/*
	PhysBridge - C0.1 spike. See PhysBridge.h for what it is asking and why.

	The flow, and every line of it is on AE's UI thread except where marked:

	  client writes {"cmd":"read_scene","script":"...jsx"} to the RX pipe
	    -> PipeServerThread reads the line              [background thread]
	    -> QueuePush                                    [background thread]
	    -> IdleHook drains the queue                    [AE UI thread]
	    -> ReadFileUtf8 + AEGP_ExecuteScript            [AE UI thread]
	    -> PipeWrite of the script's return value       [AE UI thread]
	  client reads one newline-terminated line from TX

  C0.2 adds size_probe and send_payload, which take exactly that route.
  They differ only in what reaches AEGP_ExecuteScript: a payload escaped
  into the script text, or a path the script opens for itself.
*/

#include "PhysBridge.h"

static SPBasicSuite			*sP					= NULL;
static AEGP_PluginID		S_my_id				= 0L;
static AEGP_Command			S_status_cmd		= 0L;
static AEGP_Command			S_launch_cmd		= 0L;
static char					S_app_path[1024]	= { 0 };

static HANDLE				S_pipe_tx			= INVALID_HANDLE_VALUE;
static HANDLE				S_pipe_rx			= INVALID_HANDLE_VALUE;
static HANDLE				S_pipe_thread		= NULL;
static HANDLE				S_pipe_stop_evt		= NULL;
static CRITICAL_SECTION		S_cs;
static A_Boolean			S_cs_ready			= FALSE;
static A_Boolean			S_connected			= FALSE;

static BridgeRequest		S_queue[PHYSBRIDGE_QUEUE_LEN];
static int					S_q_head			= 0;
static int					S_q_tail			= 0;

//	Spike instrumentation. These are the numbers the spike exists to produce, so
//	they are kept where the status menu item can show them.
static long					S_requests			= 0;
static long					S_last_result_len	= 0;
static long					S_last_ms			= 0;
static char					S_last_error[512]	= { 0 };

// ---------------------------------------------------------------------------
//	log
// ---------------------------------------------------------------------------

static void
Log(const char *fmtZ, ...)
{
	char	line[2048];
	va_list	ap;

	va_start(ap, fmtZ);
	vsnprintf(line, sizeof(line), fmtZ, ap);
	va_end(ap);

	OutputDebugStringA(line);

	//	A file as well as the debugger, because the interesting runs are the
	//	ones where AE is not attached to anything.
	char path[MAX_PATH];
	DWORD n = GetTempPathA(sizeof(path), path);

	if (n && n < sizeof(path) - 24) {
		strcat_s(path, sizeof(path), "physbridge.log");
		FILE *f = NULL;
		if (!fopen_s(&f, path, "a") && f) {
			fputs(line, f);
			fclose(f);
		}
	}
}

// ---------------------------------------------------------------------------
//	the request queue: written on the pipe thread, drained on AE's UI thread
// ---------------------------------------------------------------------------

//	Returns whether the request was taken. The caller keeps ownership of
//	rP->dataP if it was not -- see BridgeRequest.
static A_Boolean
QueuePush(const BridgeRequest *rP)
{
	A_Boolean took = FALSE;

	if (!S_cs_ready) {
		return FALSE;
	}
	EnterCriticalSection(&S_cs);

	int next = (S_q_tail + 1) % PHYSBRIDGE_QUEUE_LEN;

	if (next != S_q_head) {		//	drop rather than overwrite a pending one
		S_queue[S_q_tail] = *rP;
		S_q_tail = next;
		took = TRUE;
	}
	LeaveCriticalSection(&S_cs);
	return took;
}

static A_Boolean
QueuePop(BridgeRequest *outP)
{
	A_Boolean got = FALSE;

	if (!S_cs_ready) {
		return FALSE;
	}
	EnterCriticalSection(&S_cs);

	if (S_q_head != S_q_tail) {
		*outP = S_queue[S_q_head];
		S_q_head = (S_q_head + 1) % PHYSBRIDGE_QUEUE_LEN;
		got = TRUE;
	}
	LeaveCriticalSection(&S_cs);
	return got;
}

// ---------------------------------------------------------------------------
//	the smallest JSON reader that can serve this protocol
// ---------------------------------------------------------------------------

//	Pulls a string value for `keyZ` (given with its quotes and colon, e.g.
//	"\"cmd\":"). Handles \" and \\ so a Windows path survives; nothing else,
//	because nothing else is ever sent.
static A_Boolean
JsonStr(const char *lineZ, const char *keyZ, char *outZ, size_t out_max)
{
	const char *k = strstr(lineZ, keyZ);

	if (!k) {
		return FALSE;
	}
	const char *p = k + strlen(keyZ);

	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p != '"') {
		return FALSE;
	}
	p++;

	size_t o = 0;

	while (*p && *p != '"' && o + 1 < out_max) {
		if (*p == '\\' && p[1]) {
			p++;
			//	The only escapes the client ever emits.
			if (*p == 'n') {
				outZ[o++] = '\n';
			} else {
				outZ[o++] = *p;			// \\ and \"
			}
			p++;
		} else {
			outZ[o++] = *p++;
		}
	}
	outZ[o] = 0;
	return (*p == '"') ? TRUE : FALSE;
}

/*	The same reader, for a value too big to land in a fixed field.

	JsonStr's whole virtue is that it cannot overrun a caller's buffer; the
	price is that it silently stops at the end of one. That is right for a path
	and wrong for a payload, where stopping early IS the failure being measured
	-- so this one allocates and reports the length it actually decoded.

	Escapes are handled identically to JsonStr, and deliberately no more: a
	real bake is full of \" and \\, so those two decide whether this works at
	all, and inventing \u handling here that JsonStr does not have would make
	the two readers disagree about the same wire format. */
static char *
JsonStrAlloc(const char *lineZ, const char *keyZ, size_t *out_lenP)
{
	const char *k = strstr(lineZ, keyZ);

	if (!k) {
		return NULL;
	}
	const char *p = k + strlen(keyZ);

	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p != '"') {
		return NULL;
	}
	p++;

	//	The decoded value can only be shorter than what remains of the line.
	size_t	cap  = strlen(p) + 1;
	char	*outP = (char *)malloc(cap);

	if (!outP) {
		return NULL;
	}
	size_t o = 0;

	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
			outP[o++] = (*p == 'n') ? '\n' : *p;	// \\ and \" and \n
			p++;
		} else {
			outP[o++] = *p++;
		}
	}
	outP[o] = 0;

	if (*p != '"') {			//	unterminated: a truncated line, which is
		free(outP);				//	exactly what this exists to detect
		return NULL;
	}
	if (out_lenP) {
		*out_lenP = o;
	}
	return outP;
}

// ---------------------------------------------------------------------------
//	pipe: write side (UI thread only)
// ---------------------------------------------------------------------------

static void
PipeWrite(const char *bufP, size_t len)
{
	HANDLE h = INVALID_HANDLE_VALUE;

	if (S_cs_ready) {
		EnterCriticalSection(&S_cs);
		h = S_pipe_tx;
		LeaveCriticalSection(&S_cs);
	}
	if (h == INVALID_HANDLE_VALUE) {
		Log("  write: no client connected, %llu bytes dropped\n",
			(unsigned long long)len);
		return;
	}

	//	Loop: a large scene document will not go out in one WriteFile once it
	//	exceeds the pipe buffer, and a short write that is treated as a whole
	//	one is exactly how a payload-size spike gets a false pass.
	size_t sent = 0;

	while (sent < len) {
		DWORD wrote = 0;
		DWORD chunk = (DWORD)((len - sent > 0x8000) ? 0x8000 : (len - sent));

		if (!WriteFile(h, bufP + sent, chunk, &wrote, NULL) || !wrote) {
			Log("  write: failed at %llu/%llu, err=%lu\n",
				(unsigned long long)sent, (unsigned long long)len,
				(unsigned long)GetLastError());
			return;
		}
		sent += wrote;
	}
	FlushFileBuffers(h);
}

static void
PipeWriteLine(const char *sZ)
{
	PipeWrite(sZ, strlen(sZ));
	PipeWrite("\n", 1);
}

//	Errors go back as a JSON object so the client never has to guess whether a
//	line is a payload or a complaint.
static void
PipeWriteError(const char *whatZ)
{
	char msg[640];

	strncpy_s(S_last_error, sizeof(S_last_error), whatZ, _TRUNCATE);

	//	Quotes and backslashes escaped: the commonest error text is a path.
	char esc[512];
	size_t o = 0;

	for (const char *p = whatZ; *p && o + 2 < sizeof(esc); p++) {
		if (*p == '"' || *p == '\\') {
			esc[o++] = '\\';
		}
		esc[o++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
	}
	esc[o] = 0;

	sprintf_s(msg, sizeof(msg), "{\"ok\":false,\"error\":\"%s\"}", esc);
	PipeWriteLine(msg);
}

// ---------------------------------------------------------------------------
//	running the script  (UI thread only -- no AEGP call may run off it)
// ---------------------------------------------------------------------------

static char *
ReadFileUtf8(const char *pathZ, size_t *lenP)
{
	FILE *f = NULL;

	if (fopen_s(&f, pathZ, "rb") || !f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (n < 0) {
		fclose(f);
		return NULL;
	}
	char *buf = (char *)malloc((size_t)n + 1);

	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = 0;

	//	Skip a UTF-8 BOM: ExtendScript tolerates one, but it would land in the
	//	middle of the string we hand to AEGP_ExecuteScript.
	char *start = buf;

	if (got >= 3 && (unsigned char)buf[0] == 0xEF
				 && (unsigned char)buf[1] == 0xBB
				 && (unsigned char)buf[2] == 0xBF) {
		start += 3;
		got -= 3;
	}
	if (start != buf) {
		memmove(buf, start, got + 1);
	}
	if (lenP) {
		*lenP = got;
	}
	return buf;
}

/*	Put back what the prelude changed.

	Every script the bridge runs is prefixed with globals -- PHYS_RETURN_JSON
	for the reader, PHYS_P and friends for the probes -- and ExtendScript's
	global scope is not scoped to the script. It lives as long as the AE
	session, shared with every script the user runs by hand.

	Two things go wrong if it is left alone, and the first one cost a test:

	  - b1_read_shapes.jsx decides between "save through a dialog" and "return
	    the string" by testing PHYS_RETURN_JSON. Leave it set and a MANUAL run
	    keeps taking the bridge path: it returns the string, saves nothing, and
	    shows no dialog, with no error to explain why.
	  - PHYS_P holds the probe payload, so a 32 MB probe leaves 32 MB alive in
	    AE for the rest of the session.

	Cleared in its own ExecuteScript rather than appended to the script, because
	an appended line does not run when the script throws -- which is exactly
	when a stale flag does the most damage. Assignment rather than `var`: the
	reader's guard is `typeof PHYS_RETURN_JSON !== "undefined" && ...`, and a
	value of false fails its second half. */
static void
ClearBridgeGlobals(AEGP_SuiteHandler &suites)
{
	//	Both, because ERR2 folds err2 into err. Neither is inspected: a failure
	//	to clear is worth no reply of its own, and must not overwrite the
	//	caller's, which is the actual result.
	A_Err			err		= A_Err_NONE,
					err2	= A_Err_NONE;
	AEGP_MemHandle	resultH	= NULL,
					errorH	= NULL;

	ERR2(suites.UtilitySuite6()->AEGP_ExecuteScript(S_my_id,
			"PHYS_RETURN_JSON = false; PHYS_P = null; PHYS_F = null; "
			"PHYS_EVAL = false; PHYS_ECHO = false;",
			FALSE, &resultH, &errorH));

	if (resultH) {
		ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(resultH));
	}
	if (errorH) {
		ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(errorH));
	}
}

static void
DoReadScene(AEGP_SuiteHandler &suites, const char *scriptPathZ)
{
	A_Err			err		= A_Err_NONE,
					err2	= A_Err_NONE;
	A_Boolean		avail	= FALSE;
	AEGP_MemHandle	resultH	= NULL,
					errorH	= NULL;

	ERR(suites.UtilitySuite6()->AEGP_IsScriptingAvailable(&avail));

	if (err || !avail) {
		PipeWriteError("scripting is disabled in AE preferences "
						"(Preferences > Scripting & Expressions)");
		return;
	}

	size_t	code_len = 0;
	char	*codeZ = ReadFileUtf8(scriptPathZ, &code_len);

	if (!codeZ) {
		char m[MAX_PATH + 64];
		sprintf_s(m, sizeof(m), "cannot read script: %s", scriptPathZ);
		PipeWriteError(m);
		return;
	}
	Log("  read_scene: %s (%llu bytes of script)\n", scriptPathZ,
		(unsigned long long)code_len);

	/*	The script is run VERBATIM apart from this one prepended line, which
		flips it from "save through a dialog" to "return the string". Keeping
		the change to a prelude rather than a second copy of the script is what
		makes the pass criterion mean anything: both paths build the JSON with
		the same code, and only its destination differs.

		It also matters that the script never raises a modal in this mode -- an
		alert from a script started by the idle hook would block AE waiting for
		a click nobody is there to give. b1_read_shapes.jsx guards for it. */
	static const char PRELUDE[] = "var PHYS_RETURN_JSON = true;\n";

	size_t	total = sizeof(PRELUDE) - 1 + code_len;
	char	*fullZ = (char *)malloc(total + 1);

	if (!fullZ) {
		free(codeZ);
		PipeWriteError("out of memory building the script");
		return;
	}
	memcpy(fullZ, PRELUDE, sizeof(PRELUDE) - 1);
	memcpy(fullZ + sizeof(PRELUDE) - 1, codeZ, code_len);
	fullZ[total] = 0;
	free(codeZ);
	codeZ = fullZ;

	DWORD t0 = GetTickCount();

	ERR(suites.UtilitySuite6()->AEGP_ExecuteScript(S_my_id, codeZ, FALSE,
													&resultH, &errorH));
	S_last_ms = (long)(GetTickCount() - t0);
	free(codeZ);

	//	Before any of the early returns below, so it happens on every path.
	ClearBridgeGlobals(suites);

	//	The documented trap: on SUCCESS this handle is non-NULL but its string
	//	is empty. Testing the handle alone reports a failure on every good run.
	if (errorH) {
		A_char *t = NULL;

		if (!suites.MemorySuite1()->AEGP_LockMemHandle(errorH,
					reinterpret_cast<void**>(&t)) && t && t[0]) {
			Log("  read_scene ERROR: %s\n", t);
			PipeWriteError(t);
			ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(errorH));
			ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(errorH));

			if (resultH) {
				ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(resultH));
			}
			return;
		}
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(errorH));
		ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(errorH));
	}
	if (err) {
		PipeWriteError("AEGP_ExecuteScript failed");
		return;
	}
	if (!resultH) {
		PipeWriteError("script returned no value -- it must RETURN the JSON, "
						"not save it");
		return;
	}

	A_char *t = NULL;

	if (!suites.MemorySuite1()->AEGP_LockMemHandle(resultH,
				reinterpret_cast<void**>(&t)) && t) {
		size_t n = strlen(t);
		S_last_result_len = (long)n;

		//	Streamed straight out. No fixed buffer anywhere in this path: the
		//	size of what comes back IS the measurement (C0.2).
		Log("  read_scene: %llu bytes returned in %ld ms\n",
			(unsigned long long)n, S_last_ms);

		if (!n || 0 == strcmp(t, "undefined")) {
			PipeWriteError("script returned 'undefined' -- it must RETURN the "
							"JSON string");
		} else if (0 == strncmp(t, "ERROR: ", 7)) {
			//	The script's own refusals -- no comp selected, no usable shape
			//	layers. They arrive as a string because raising a modal would
			//	hang the idle hook; they are still failures.
			PipeWriteError(t + 7);
		} else {
			PipeWrite(t, n);
			PipeWrite("\n", 1);
			S_last_error[0] = 0;
		}
	} else {
		PipeWriteError("could not lock the script result");
	}
	ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(resultH));
	ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(resultH));
}

// ---------------------------------------------------------------------------
//	C0.2 -- payload size  (UI thread only)
// ---------------------------------------------------------------------------

/*	THE QUESTION, in the form it actually takes.

	B2 already reads its bake from a file: File.openDialog -> read() -> eval.
	So "must the script read a temp file?" is not asking whether the file path
	WORKS -- it demonstrably does, by hand, today. It is asking whether the
	alternative, handing the bake to AEGP_ExecuteScript as part of the script
	text, is viable and buys anything. If it is not, B2 needs the same one-line
	prelude B1 got and nothing more.

	So the probe measures both ingestion paths on the SAME payload:

	  literal   the bytes are escaped into a string literal in the script
	  file      the script opens a path and reads it

	and it measures them on the real b2_bake.json, not only on synthetic bytes,
	because 145,090 bytes of numeric JSON is the payload the answer is for.

	INTEGRITY, NOT JUST SURVIVAL. A truncated payload that still parses is how
	this question gets a false pass, so the script checksums what it received
	and the bridge compares that against a checksum of what it sent. Length
	alone would miss a mangled escape; a checksum alone would miss nothing, but
	head and tail markers say WHERE a truncation happened, which is the
	difference between a number and a diagnosis. */

//	The rolling checksum, defined here and in PROBE_BODY below, and the two must
//	stay identical. It runs over UTF-16 code units on the script side, so it can
//	only agree for ASCII input -- the caller is told when the payload is not.
static unsigned long
ChecksumAscii(const char *sP, size_t len)
{
	unsigned long long h = 0;

	for (size_t i = 0; i < len; i++) {
		h = (h * 31 + (unsigned char)sP[i]) % 4294967296ULL;
	}
	return (unsigned long)h;
}

static A_Boolean
IsAscii(const char *sP, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if ((unsigned char)sP[i] > 0x7F) {
			return FALSE;
		}
	}
	return TRUE;
}

//	Char codes as hex, the same way the script reports its own head and tail.
//	Hex rather than the characters themselves because the payload is JSON: its
//	first bytes are a brace and a quote, and pasting those into a reply would be
//	an injection into the very format the reply is written in.
static void
HexCodes(const char *sP, size_t len, char *outZ, size_t out_max)
{
	size_t o = 0;

	for (size_t i = 0; i < len && o + 5 < out_max; i++) {
		sprintf_s(outZ + o, out_max - o, "%04x", (unsigned char)sP[i]);
		o += 4;
	}
	outZ[o] = 0;
}

//	Escape a byte range into the body of a JavaScript string literal. Worst case
//	is six bytes out per byte in, which is what the allocation assumes. Bytes
//	>= 0x80 pass through untouched: the script text reaches AE as UTF-8 and a
//	multi-byte sequence must survive whole.
static char *
EscapeToLiteral(const char *srcP, size_t len, size_t *out_lenP)
{
	size_t	cap  = len * 6 + 1;
	char	*dstP = (char *)malloc(cap);

	if (!dstP) {
		return NULL;
	}
	size_t o = 0;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)srcP[i];

		switch (c) {
			case '\\':	dstP[o++] = '\\'; dstP[o++] = '\\';	break;
			case '"':	dstP[o++] = '\\'; dstP[o++] = '"';	break;
			case '\n':	dstP[o++] = '\\'; dstP[o++] = 'n';	break;
			case '\r':	dstP[o++] = '\\'; dstP[o++] = 'r';	break;
			case '\t':	dstP[o++] = '\\'; dstP[o++] = 't';	break;
			default:
				if (c < 0x20) {
					sprintf_s(dstP + o, cap - o, "\\u%04x", c);
					o += 6;
				} else {
					dstP[o++] = (char)c;
				}
		}
	}
	dstP[o] = 0;

	if (out_lenP) {
		*out_lenP = o;
	}
	return dstP;
}

/*	The half of the probe script that does not depend on how the payload got
	there. Everything above it must leave the payload in PHYS_P.

	ms_sum and ms_eval are timed separately from the bridge's own stopwatch,
	because they answer a different question: the bridge measures the cost of
	the whole transfer, and these measure how much of it is ExtendScript being
	ExtendScript. If eval of 145 KB dominates, the choice between literal and
	file barely matters and the spike should say so.

	eval is what B2 already does with the bake, so parsing here is not extra
	work invented for the probe -- it is the cost B2 pays either way, and a
	payload that arrives intact but cannot be parsed is still a failure. */
static const char PROBE_BODY[] =
	"(function(){\n"
	"  function hx(s){var o='';for(var i=0;i<s.length;i++){"
	"var c=s.charCodeAt(i).toString(16);"
	"while(c.length<4)c='0'+c;o+=c;}return o;}\n"
	"  var n=PHYS_P.length;\n"
	"  var t0=new Date().getTime();\n"
	"  var h=0;\n"
	"  for(var i=0;i<n;i++)h=(h*31+PHYS_P.charCodeAt(i))%4294967296;\n"
	"  var t1=new Date().getTime();\n"
	"  var ev=-1,keys=-1,perr='';\n"
	"  if(PHYS_EVAL){\n"
	"    var t2=new Date().getTime();\n"
	"    try{var ob=eval('('+PHYS_P+')');keys=0;for(var k in ob)keys++;}\n"
	"    catch(e){perr=String(e.message||e);}\n"
	"    ev=new Date().getTime()-t2;\n"
	"  }\n"
	"  var s='{\"chars\":'+n+',\"sum\":'+h+',\"ms_sum\":'+(t1-t0)\n"
	"    +',\"ms_eval\":'+ev+',\"keys\":'+keys\n"
	"    +',\"head\":\"'+hx(PHYS_P.substr(0,16))+'\"'\n"
	"    +',\"tail\":\"'+hx(PHYS_P.substr(n>16?n-16:0))+'\"'\n"
	"    +',\"parse_error\":\"'+perr.replace(/[\\\\\"]/g,' ')+'\"}';\n"
	"  if(PHYS_ECHO)return PHYS_P;\n"
	"  return s;\n"
	"})()\n";

//	Synthetic bytes for the sweep: a cycle of safe ASCII. A repeating pattern
//	would hide a reordering, which is what the checksum is for, and the sweep is
//	asking about the ceiling rather than fidelity -- the real bake is what
//	fidelity gets measured on.
static char *
MakeSynthetic(size_t n)
{
	static const char ALPHA[] =
		"0123456789abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ.,:;-_/";
	const size_t	na	= sizeof(ALPHA) - 1;
	char			*bP	= (char *)malloc(n + 1);

	if (!bP) {
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		bP[i] = ALPHA[i % na];
	}
	bP[n] = 0;
	return bP;
}

/*	Run one probe.

	payloadP is what the script must end up holding. In file mode it is NOT
	sent -- it is written to a temp file and the script reads it back -- so the
	checksum still compares what the bridge intended against what the script
	saw, and both modes are measured against the same ruler.

	The reply is a JSON object rather than a bare payload, because the numbers
	ARE the result here. echo is the one exception and returns the payload, so
	the return direction can be measured on its own. */
static void
RunProbe(AEGP_SuiteHandler	&suites,
		 const char			*modeZ,
		 const char			*payloadP,
		 size_t				 payload_len,
		 A_Boolean			 do_eval,
		 A_Boolean			 do_echo)
{
	A_Err			err		= A_Err_NONE,
					err2	= A_Err_NONE;
	AEGP_MemHandle	resultH	= NULL,
					errorH	= NULL;
	A_Boolean		is_file	= (0 == strcmp(modeZ, "file"));
	char			*headerP = NULL;

	//	What was sent, measured before anything can mangle it.
	unsigned long	sum_sent	= ChecksumAscii(payloadP, payload_len);
	A_Boolean		ascii		= IsAscii(payloadP, payload_len);
	size_t			edge		= (payload_len < 16) ? payload_len : 16;
	char			head_sent[80], tail_sent[80];

	HexCodes(payloadP, edge, head_sent, sizeof(head_sent));
	HexCodes(payloadP + payload_len - edge, edge, tail_sent, sizeof(tail_sent));

	if (is_file) {
		char	dirZ[MAX_PATH];
		char	tmpZ[MAX_PATH];

		if (!GetTempPathA(sizeof(dirZ), dirZ)) {
			PipeWriteError("GetTempPath failed");
			return;
		}
		sprintf_s(tmpZ, sizeof(tmpZ), "%sphysbridge_c02_payload.json", dirZ);

		FILE *f = NULL;

		if (fopen_s(&f, tmpZ, "wb") || !f) {
			PipeWriteError("cannot write the temp payload file");
			return;
		}
		size_t wrote = fwrite(payloadP, 1, payload_len, f);
		fclose(f);

		if (wrote != payload_len) {
			PipeWriteError("short write to the temp payload file");
			return;
		}

		//	Forward slashes: ExtendScript's File takes a URI-ish path, and a
		//	backslash inside a literal is an escape waiting to be misread.
		char	fwdZ[MAX_PATH];
		size_t	fi = 0;

		for (const char *p = tmpZ; *p && fi + 1 < sizeof(fwdZ); p++) {
			fwdZ[fi++] = (*p == '\\') ? '/' : *p;
		}
		fwdZ[fi] = 0;

		size_t cap = MAX_PATH + 256;
		headerP = (char *)malloc(cap);

		if (headerP) {
			sprintf_s(headerP, cap,
				"var PHYS_EVAL=%s;var PHYS_ECHO=%s;\n"
				"var PHYS_F=new File(\"%s\");PHYS_F.encoding=\"UTF-8\";\n"
				"PHYS_F.open(\"r\");var PHYS_P=PHYS_F.read();PHYS_F.close();\n",
				do_eval ? "true" : "false", do_echo ? "true" : "false", fwdZ);
		}
	} else {
		size_t	esc_len	= 0;
		char	*escP	= EscapeToLiteral(payloadP, payload_len, &esc_len);

		if (!escP) {
			PipeWriteError("out of memory escaping the payload");
			return;
		}
		size_t cap = esc_len + 128;
		headerP = (char *)malloc(cap);

		if (headerP) {
			int k = sprintf_s(headerP, cap,
				"var PHYS_EVAL=%s;var PHYS_ECHO=%s;var PHYS_P=\"",
				do_eval ? "true" : "false", do_echo ? "true" : "false");
			memcpy(headerP + k, escP, esc_len);
			memcpy(headerP + k + esc_len, "\";\n", 4);
		}
		free(escP);
	}
	if (!headerP) {
		PipeWriteError("out of memory building the probe script");
		return;
	}

	size_t	head_len	= strlen(headerP);
	size_t	script_len	= head_len + sizeof(PROBE_BODY) - 1;
	char	*scriptP	= (char *)malloc(script_len + 1);

	if (!scriptP) {
		free(headerP);
		PipeWriteError("out of memory assembling the probe script");
		return;
	}
	memcpy(scriptP, headerP, head_len);
	memcpy(scriptP + head_len, PROBE_BODY, sizeof(PROBE_BODY));
	free(headerP);

	Log("  probe: mode=%s payload=%llu bytes, script=%llu bytes\n",
		modeZ, (unsigned long long)payload_len,
		(unsigned long long)script_len);

	DWORD t0 = GetTickCount();

	ERR(suites.UtilitySuite6()->AEGP_ExecuteScript(S_my_id, scriptP, FALSE,
													&resultH, &errorH));
	S_last_ms = (long)(GetTickCount() - t0);
	free(scriptP);

	//	Timed OUTSIDE the stopwatch above, so the measurement stays a
	//	measurement of the transfer. PHYS_P holds the payload, so skipping this
	//	would leave a 32 MB probe's bytes alive in AE for the whole session.
	ClearBridgeGlobals(suites);

	//	The same trap as C0.1: on success this handle is non-NULL with an EMPTY
	//	string, so the string decides, never the handle.
	if (errorH) {
		A_char *t = NULL;

		if (!suites.MemorySuite1()->AEGP_LockMemHandle(errorH,
					reinterpret_cast<void**>(&t)) && t && t[0]) {
			Log("  probe ERROR at %llu bytes: %s\n",
				(unsigned long long)payload_len, t);
			PipeWriteError(t);
			ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(errorH));
			ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(errorH));

			if (resultH) {
				ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(resultH));
			}
			return;
		}
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(errorH));
		ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(errorH));
	}
	if (err) {
		//	The ceiling, if there is one, is expected to appear here.
		char m[192];
		sprintf_s(m, sizeof(m),
			"AEGP_ExecuteScript failed (err %ld) at %llu bytes of payload, "
			"%llu bytes of script", (long)err,
			(unsigned long long)payload_len, (unsigned long long)script_len);
		PipeWriteError(m);
		return;
	}
	if (!resultH) {
		PipeWriteError("the probe script returned no value");
		return;
	}

	A_char *t = NULL;

	if (!suites.MemorySuite1()->AEGP_LockMemHandle(resultH,
				reinterpret_cast<void**>(&t)) && t) {
		size_t n = strlen(t);
		S_last_result_len = (long)n;

		Log("  probe: %llu bytes back in %ld ms\n",
			(unsigned long long)n, S_last_ms);

		if (do_echo) {
			//	The return direction, measured alone. The reply IS the payload,
			//	so the client checksums it rather than reading a verdict.
			PipeWrite(t, n);
			PipeWrite("\n", 1);
		} else {
			/*	The script's report and the bridge's own account of what it
				sent, in one object. The client compares the two halves; the
				bridge deliberately does not pronounce a verdict, because
				"sum_sent != sum" and "chars != bytes_sent" mean different
				things and only one of them is a failure. */
			size_t	cap		= n + 768;
			char	*replyP	= (char *)malloc(cap);

			if (replyP) {
				sprintf_s(replyP, cap,
					"{\"ok\":true,\"mode\":\"%s\",\"bytes_sent\":%llu,"
					"\"script_bytes\":%llu,\"ms\":%ld,\"ascii\":%s,"
					"\"sum_sent\":%lu,\"head_sent\":\"%s\","
					"\"tail_sent\":\"%s\",\"script\":%s}",
					modeZ, (unsigned long long)payload_len,
					(unsigned long long)script_len, S_last_ms,
					ascii ? "true" : "false", sum_sent, head_sent, tail_sent,
					t);
				PipeWriteLine(replyP);
				free(replyP);
			} else {
				PipeWriteError("out of memory building the probe reply");
			}
		}
		S_last_error[0] = 0;
	} else {
		PipeWriteError("could not lock the probe result");
	}
	ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(resultH));
	ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(resultH));
}

//	{"cmd":"size_probe","bytes":"N","mode":"literal"|"file","echo":"1"}
static void
DoSizeProbe(AEGP_SuiteHandler &suites, const BridgeRequest *rP)
{
	size_t n = (size_t)strtoull(rP->num, NULL, 10);

	if (!n) {
		PipeWriteError("size_probe needs a positive \"bytes\"");
		return;
	}
	char *bufP = MakeSynthetic(n);

	if (!bufP) {
		//	Said explicitly, because a spike that reports the harness running
		//	out of memory as AE's ceiling has measured nothing.
		char m[128];
		sprintf_s(m, sizeof(m),
			"the BRIDGE could not allocate %llu bytes -- this is the probe's "
			"limit, not AE's", (unsigned long long)n);
		PipeWriteError(m);
		return;
	}
	//	No eval on synthetic bytes: they are not JSON, and the sweep asks about
	//	the transfer ceiling, not about the parser.
	RunProbe(suites, rP->mode[0] ? rP->mode : "literal", bufP, n,
			 FALSE, rP->echo);
	free(bufP);
}

/*	{"cmd":"pipe_probe","data":"<the payload, inline>"}

	C0.2 measured AEGP_ExecuteScript and said plainly that it had NOT measured
	the pipe, because the payload never crossed it -- it was made or read inside
	the bridge. This is that missing half: the payload arrives in the request
	line itself, which is what a shell handing over a bake would actually do.

	It touches no AEGP suite, so what it reports is the transport and the
	request reader and nothing else. It still runs on the UI thread, with the
	rest, so that a large request cannot be measured on a quieter thread than
	the one it would really be served on. */
static void
DoPipeProbe(const BridgeRequest *rP)
{
	if (!rP->dataP) {
		PipeWriteError("pipe_probe needs a \"data\" string -- none arrived, "
						"which for a large request usually means the line was "
						"cut before its closing quote");
		return;
	}
	size_t			n		= rP->data_len;
	size_t			edge	= (n < 16) ? n : 16;
	char			head[80], tail[80];

	HexCodes(rP->dataP, edge, head, sizeof(head));
	HexCodes(rP->dataP + n - edge, edge, tail, sizeof(tail));

	char msg[512];

	sprintf_s(msg, sizeof(msg),
		"{\"ok\":true,\"bytes_received\":%llu,\"sum\":%lu,"
		"\"ascii\":%s,\"head\":\"%s\",\"tail\":\"%s\"}",
		(unsigned long long)n, ChecksumAscii(rP->dataP, n),
		IsAscii(rP->dataP, n) ? "true" : "false", head, tail);

	S_last_result_len = (long)n;
	Log("  pipe_probe: %llu bytes received inline\n", (unsigned long long)n);
	PipeWriteLine(msg);
}

//	{"cmd":"send_payload","script":"<path>","mode":"literal"|"file"}
//	The real one: b2_bake.json through both ingestion paths.
static void
DoSendPayload(AEGP_SuiteHandler &suites, const BridgeRequest *rP)
{
	size_t	len	  = 0;
	char	*bufP = ReadFileUtf8(rP->arg, &len);

	if (!bufP) {
		char m[MAX_PATH + 64];
		sprintf_s(m, sizeof(m), "cannot read payload: %s", rP->arg);
		PipeWriteError(m);
		return;
	}
	if (!len) {
		free(bufP);
		PipeWriteError("the payload file is empty");
		return;
	}
	//	This payload IS JSON, so it is parsed -- that is the cost B2 pays.
	RunProbe(suites, rP->mode[0] ? rP->mode : "literal", bufP, len,
			 TRUE, rP->echo);
	free(bufP);
}

// ---------------------------------------------------------------------------
//	C0.3 -- keyframes from native code  (UI thread only)
// ---------------------------------------------------------------------------

/*	THE QUESTION.

	B2 measured, on 6,486 real keyframes, that writing VALUES is free and making
	them MEAN what we meant is not:

	    setValueAtTime in a loop      6,850 us/key
	    setValuesAtTimes in bulk         19.6 us/key
	    forcing LINEAR interpolation    853 us/key   <- no bulk form, not optional

	So Wall I is the interpolation pass, and C0.3 asks whether AEGP_KeyframeSuite
	beats it. Reading the suite sharpens the question before a line runs:
	AEGP_SetKeyframeInterpolation is ALSO per-key, with no bulk form. The native
	path is the same shape as the ExtendScript one.

	Which means the spike is really asking: is 853 us/key the ExtendScript
	BRIDGE, or is it AE doing the work? Only the first goes away by going
	native. C0.2 found ExtendScript's per-character cost dominated everything
	else, which is a reason to suspect the bridge -- but suspicion is not a
	measurement, and a per-CALL tax is not a per-character one.

	WHY IT BUILDS ITS OWN COMP.

	The benchmark writes tens of thousands of keyframes. Doing that to whatever
	the user has open would be rude at best, and it would also make the numbers
	depend on their project. So it creates a scratch comp and a solid, measures
	those, and deletes them -- inside ONE undo group, so anything it leaves
	behind is a single Undo away.

	WHY IT DOES NOT USE GetTickCount.

	The rest of the bridge times with GetTickCount, whose ~15.6 ms resolution is
	why every C0.2 number is a multiple of about fifteen. That is fine for a
	100 ms transfer and useless here: at 853 us/key, a thousand keys is under
	one tick. This uses QueryPerformanceCounter. */

static double
NowSeconds(void)
{
	LARGE_INTEGER	f, t;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / (double)f.QuadPart;
}

/*	Tear down whatever got built, in reverse, and close the undo group.

	One function called from every exit, because a spike that strands a scratch
	comp on the failure path teaches the user to distrust it -- and the failure
	path is the one that runs when something is wrong. It owns the undo group
	for the same reason: four call sites each remembering to end it in the
	right order is four chances to get it wrong.

	THE ORDER MATTERS, and getting it wrong is what took a machine down.

	Every call in AEGP_KeyframeSuite is marked UNDOABLE. The first version put
	the keyframe work AND the comp deletion in one group, so AE had to retain
	every operation plus a comp it might have to resurrect. The measured work
	now closes first, and the teardown gets its own small group.

	That bounds the SHAPE of what is retained; it does not empty it. Only a
	purge does, and app.purge(UNDO_CACHES) throws away the user's undo history
	for their whole project -- so it is opt-in, never the default. Wiping
	somebody's undo stack to tidy up after a benchmark is not a trade this code
	gets to make on their behalf. */
static void
BenchFinish(AEGP_SuiteHandler	&suites,
			AEGP_StreamRefH		streamH,
			AEGP_ItemH			comp_itemH,
			AEGP_ItemH			solid_itemH,
			A_Boolean			do_purge)
{
	A_Err err2 = A_Err_NONE, err = A_Err_NONE;

	if (streamH) {
		ERR2(suites.StreamSuite2()->AEGP_DisposeStream(streamH));
	}

	//	Close the measured group BEFORE anything else touches the project.
	ERR2(suites.UtilitySuite3()->AEGP_EndUndoGroup());

	if (comp_itemH || solid_itemH) {
		ERR2(suites.UtilitySuite3()->AEGP_StartUndoGroup(
				"PhysBridge C0.3 cleanup"));

		if (comp_itemH) {
			ERR2(suites.ItemSuite6()->AEGP_DeleteItem(comp_itemH));
		}
		if (solid_itemH) {
			ERR2(suites.ItemSuite6()->AEGP_DeleteItem(solid_itemH));
		}
		ERR2(suites.UtilitySuite3()->AEGP_EndUndoGroup());
	}

	if (do_purge) {
		AEGP_MemHandle resultH = NULL, errorH = NULL;

		/*	Guarded individually: PurgeTarget is not on every AE version, and
			an exception here would abandon the rest. No alert() anywhere --
			this runs inside the idle hook, where a modal waits for a click
			nobody is there to give. */
		ERR2(suites.UtilitySuite6()->AEGP_ExecuteScript(S_my_id,
				"try{app.purge(PurgeTarget.UNDO_CACHES);}catch(e){}"
				"try{app.purge(PurgeTarget.ALL_CACHES);}catch(e){}",
				FALSE, &resultH, &errorH));

		if (resultH) {
			ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(resultH));
		}
		if (errorH) {
			ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(errorH));
		}
		Log("  bench_keys: purged undo and all caches\n");
	}
}

//	{"cmd":"bench_keys","bytes":"N"}
static void
DoBenchKeys(AEGP_SuiteHandler &suites, const BridgeRequest *rP)
{
	A_Err			err		= A_Err_NONE,
					err2	= A_Err_NONE;
	AEGP_CompH		compH			= NULL;
	AEGP_ItemH		comp_itemH		= NULL,
					solid_itemH		= NULL,
					root_folderH	= NULL;
	AEGP_FootageH	footageH		= NULL;
	AEGP_LayerH		layerH			= NULL;
	AEGP_StreamRefH	streamH			= NULL;
	AEGP_ProjectH	projH			= NULL;

	long n = (long)strtol(rP->num, NULL, 10);

	/*	20,000 rather than the 200,000 this started with. The sweep that
		exhausted a machine's memory ran to 12,000 keys eight times over, and
		the useful range was never above 6,486 -- B2's real key count. Anything
		larger was extrapolation bought with retained undo state. */
	if (n < 1 || n > 20000) {
		PipeWriteError("bench_keys needs \"bytes\" between 1 and 20000 -- it "
						"is a key count, and the harness is capped because the "
						"earlier sweep exhausted memory at this scale");
		return;
	}

	//	Opt-in, and it stays that way: purging discards the user's undo
	//	history for their whole project, not just this benchmark's share.
	A_Boolean do_purge = rP->purge;

	//	One group for the whole thing, so a failure anywhere is one Undo away.
	ERR(suites.UtilitySuite3()->AEGP_StartUndoGroup("PhysBridge C0.3 bench"));

	ERR(suites.ProjSuite5()->AEGP_GetProjectByIndex(0, &projH));
	ERR(suites.ProjSuite5()->AEGP_GetProjectRootFolder(projH, &root_folderH));

	if (err) {
		ERR2(suites.UtilitySuite3()->AEGP_EndUndoGroup());
		PipeWriteError("could not reach the project root folder");
		return;
	}

	//	30 fps, and long enough to hold every key plus a little slack.
	A_Ratio	pixel_aspect	= { 1, 1 };
	A_Ratio	framerate		= { 30, 1 };
	A_Time	duration		= { n + 60, 30 };

	//	UTF-16, because AEGP_CreateComp takes it. Built by hand rather than
	//	dragging in a conversion for one ASCII literal.
	const char	*nameZ = "PhysBridge C0.3 scratch";
	A_UTF16Char	name16[64];
	int			ni = 0;

	for (; nameZ[ni] && ni < 63; ni++) {
		name16[ni] = (A_UTF16Char)nameZ[ni];
	}
	name16[ni] = 0;

	ERR(suites.CompSuite11()->AEGP_CreateComp(root_folderH, name16,
			1000, 500, &pixel_aspect, &duration, &framerate, &compH));
	ERR(suites.CompSuite11()->AEGP_GetItemFromComp(compH, &comp_itemH));

	AEGP_ColorVal white = { 1.0, 1.0, 1.0, 1.0 };

	ERR(suites.FootageSuite5()->AEGP_NewSolidFootage("c03 solid", 100, 100,
			&white, &footageH));
	ERR(suites.FootageSuite5()->AEGP_AddFootageToProject(footageH,
			root_folderH, &solid_itemH));
	ERR(suites.LayerSuite8()->AEGP_AddLayer(solid_itemH, compH, &layerH));
	ERR(suites.StreamSuite2()->AEGP_GetNewLayerStream(S_my_id, layerH,
			AEGP_LayerStream_POSITION, &streamH));

	if (err) {
		BenchFinish(suites, streamH, comp_itemH, solid_itemH, do_purge);
		PipeWriteError("could not build the scratch comp for the benchmark");
		return;
	}

	//	Reported rather than assumed: B2 was bitten by a 2D Position whose
	//	SPATIAL TANGENTS wanted three elements, so the arity belongs to the
	//	call and not to the property.
	A_short dim = 0;

	ERR2(suites.KeyframeSuite4()->AEGP_GetStreamValueDimensionality(streamH,
			&dim));

	// -- phase 1: the batch add, the native answer to setValuesAtTimes -----
	AEGP_AddKeyframesInfoH	akH = NULL;
	double					t0 = NowSeconds();

	ERR(suites.KeyframeSuite4()->AEGP_StartAddKeyframes(streamH, &akH));

	for (long i = 0; !err && i < n; i++) {
		A_Time	t		= { i, 30 };
		A_long	index	= 0;

		ERR(suites.KeyframeSuite4()->AEGP_AddKeyframes(akH,
				AEGP_LTimeMode_LayerTime, &t, &index));

		//	A ramp, so a wrong value is visible rather than plausible.
		AEGP_StreamValue2 v;

		AEFX_CLR_STRUCT(v);
		v.streamH			= streamH;
		v.val.three_d.x		= 100.0 + i;
		v.val.three_d.y		= 250.0 + (i % 100);
		v.val.three_d.z		= 0.0;

		ERR(suites.KeyframeSuite4()->AEGP_SetAddKeyframe(akH, index, &v));
	}
	ERR(suites.KeyframeSuite4()->AEGP_EndAddKeyframes(TRUE, akH));

	double add_s = NowSeconds() - t0;

	if (err) {
		BenchFinish(suites, streamH, comp_itemH, solid_itemH, do_purge);
		PipeWriteError("the batch add failed");
		return;
	}

	//	Did they all land? A benchmark over fewer keys than it thinks is a
	//	benchmark of nothing.
	A_long stored = 0;

	ERR2(suites.KeyframeSuite4()->AEGP_GetStreamNumKFs(streamH, &stored));

	// -- phase 2: interpolation, which is the row Wall I cares about -------
	t0 = NowSeconds();

	for (long i = 0; !err && i < stored; i++) {
		ERR(suites.KeyframeSuite4()->AEGP_SetKeyframeInterpolation(streamH, i,
				AEGP_KeyInterp_LINEAR, AEGP_KeyInterp_LINEAR));
	}
	double interp_s = NowSeconds() - t0;

	/*	-- phase 3: clearing spatial auto-bezier ---------------------------

		B2's makeLinear() makes THREE calls per Position key, not one:

		    setInterpolationTypeAtKey(i, LINEAR, LINEAR)
		    setSpatialAutoBezierAtKey(i, false)
		    setSpatialTangentsAtKey(i, zero, zero)

		and its 853 us/key covers all three. Timing only the first natively
		and comparing it to that number would be measuring a third of the work
		against all of it. So this phase exists to make the comparison honest,
		not because anyone asked for it. */
	/*	Skippable, because the first run showed this call is where the whole
		cost lives and that its per-key price GROWS with the key count --
		167 us/key at 1,000 and 2,765 at 12,000, which is O(n^2) overall.
		Something inside it walks the keyframe list every time.

		So `mode: "nobezier"` omits it, and the flag read-back below then
		answers the question that decides everything: does
		AEGP_SetKeyframeSpatialTangents clear SPATIAL_AUTOBEZIER by itself? If
		it does, this call is redundant natively and the quadratic term goes
		away. If it does not, the flag comes back SET and native loses. */
	A_Boolean	do_bezier = strcmp(rP->mode, "nobezier") != 0;
	double		bezier_s  = -1.0;

	if (do_bezier) {
		t0 = NowSeconds();

		for (long i = 0; !err && i < stored; i++) {
			ERR(suites.KeyframeSuite4()->AEGP_SetKeyframeFlag(streamH, i,
					AEGP_KeyframeFlag_SPATIAL_AUTOBEZIER, FALSE));
		}
		bezier_s = NowSeconds() - t0;
	}

	// -- phase 4: zeroing the spatial tangents ----------------------------
	AEGP_StreamValue2 zero;

	AEFX_CLR_STRUCT(zero);
	zero.streamH = streamH;

	t0 = NowSeconds();

	for (long i = 0; !err && i < stored; i++) {
		ERR(suites.KeyframeSuite4()->AEGP_SetKeyframeSpatialTangents(streamH,
				i, &zero, &zero));
	}
	double tan_s = NowSeconds() - t0;

	/*	Read one back. Fast and wrong is not a result -- and A5's lesson,
		restated by B2 inside AE, is that the fault hides between keyframes:
		every stored value can be perfect while the path bows. The
		interpolation type alone would not catch that, so the SPATIAL_AUTOBEZIER
		flag is read back too. If it is still set, the pass did not take and
		the timings describe work that achieved nothing. */
	AEGP_KeyframeInterpolationType	in_interp	= AEGP_KeyInterp_NONE,
									out_interp	= AEGP_KeyInterp_NONE;
	AEGP_KeyframeFlags				flags		= AEGP_KeyframeFlag_NONE;
	A_long							probe		= stored / 2;

	/*	Sampled, not spot-checked.

		The claim this benchmark exists to support -- that the auto-bezier call
		is redundant because setting the tangents clears the flag -- would rest
		on ONE keyframe if this read back only the middle one. B2 samples every
		17th key for exactly that reason, and the fault list it calibrates
		against includes a single dropped keyframe that a coarser check stepped
		straight over.

		So this walks a spread of keys and reports the WORST it saw. The last
		key is always included: an endpoint is where AE is most likely to treat
		a keyframe differently, and it is the cheapest place for this to be
		wrong without anyone noticing.

		The tangents themselves are read, not just the flag. A cleared flag
		with a non-zero tangent still bows the path, and that is the fault A5
		spent a whole phase learning to see. */
	double	tan_mag		= -1.0;			// the worst magnitude seen
	long	checked		= 0,
			bad_interp	= 0,
			bad_flag	= 0;

	if (stored > 0) {
		const long	WANT = 64;
		long		step = stored / WANT;

		if (step < 1) {
			step = 1;
		}
		for (long i = 0; i < stored; i += step) {
			//	Forced onto the final key on the last pass, rather than
			//	wherever the stride happens to land.
			long k = (i + step >= stored) ? (stored - 1) : i;

			AEGP_KeyframeInterpolationType	ki = AEGP_KeyInterp_NONE,
											ko = AEGP_KeyInterp_NONE;
			AEGP_KeyframeFlags				kf = AEGP_KeyframeFlag_NONE;

			ERR2(suites.KeyframeSuite4()->AEGP_GetKeyframeInterpolation(
					streamH, k, &ki, &ko));
			ERR2(suites.KeyframeSuite4()->AEGP_GetKeyframeFlags(streamH, k,
					&kf));

			if (ki != AEGP_KeyInterp_LINEAR || ko != AEGP_KeyInterp_LINEAR) {
				bad_interp++;
			}
			if (kf & AEGP_KeyframeFlag_SPATIAL_AUTOBEZIER) {
				bad_flag++;
			}
			if (k == probe) {			//	kept verbatim for the record
				in_interp	= ki;
				out_interp	= ko;
				flags		= kf;
			}

			AEGP_StreamValue2 in_tan, out_tan;

			AEFX_CLR_STRUCT(in_tan);
			AEFX_CLR_STRUCT(out_tan);

			if (!suites.KeyframeSuite4()->AEGP_GetNewKeyframeSpatialTangents(
					S_my_id, streamH, k, &in_tan, &out_tan)) {
				double a = in_tan.val.three_d.x,  b = in_tan.val.three_d.y,
					   c = out_tan.val.three_d.x, d = out_tan.val.three_d.y;
				double m = (a < 0 ? -a : a) + (b < 0 ? -b : b)
						 + (c < 0 ? -c : c) + (d < 0 ? -d : d);

				if (m > tan_mag) {
					tan_mag = m;
				}
				//	StreamSuite6: earlier versions dispose the OLD
				//	AEGP_StreamValue, which is a different type.
				ERR2(suites.StreamSuite6()->AEGP_DisposeStreamValue(&in_tan));
				ERR2(suites.StreamSuite6()->AEGP_DisposeStreamValue(&out_tan));
			}
			checked++;

			if (k == stored - 1) {
				break;
			}
		}
	}

	A_Err bench_err = err;

	BenchFinish(suites, streamH, comp_itemH, solid_itemH, do_purge);

	if (bench_err) {
		char m[128];
		sprintf_s(m, sizeof(m), "the benchmark failed part-way (err %ld) "
					"after %ld keys", (long)bench_err, (long)stored);
		PipeWriteError(m);
		return;
	}

	char msg[640];

	sprintf_s(msg, sizeof(msg),
		"{\"ok\":true,\"asked\":%ld,\"stored\":%ld,\"dim\":%d,"
		"\"add_us_per_key\":%.1f,\"interp_us_per_key\":%.1f,"
		"\"bezier_us_per_key\":%.1f,\"tangent_us_per_key\":%.1f,"
		"\"add_s\":%.3f,\"interp_s\":%.3f,\"bezier_s\":%.3f,"
		"\"tangent_s\":%.3f,\"interp_readback\":%ld,\"flags_readback\":%ld,"
		"\"tangent_magnitude\":%.4f,\"bezier_ran\":%s,"
		"\"keys_checked\":%ld,\"bad_interp\":%ld,\"bad_flag\":%ld,"
		"\"spatial_autobezier_still_set\":%s}",
		n, (long)stored, (int)dim,
		stored ? add_s	   * 1e6 / stored : 0.0,
		stored ? interp_s  * 1e6 / stored : 0.0,
		stored ? bezier_s  * 1e6 / stored : 0.0,
		stored ? tan_s	   * 1e6 / stored : 0.0,
		add_s, interp_s, bezier_s, tan_s, (long)in_interp, (long)flags,
		tan_mag, do_bezier ? "true" : "false",
		checked, bad_interp, bad_flag,
		(flags & AEGP_KeyframeFlag_SPATIAL_AUTOBEZIER) ? "true" : "false");

	Log("  bench_keys: %ld keys, add %.1f, interp %.1f, bezier %.1f, "
		"tangents %.1f us per key\n", (long)stored,
		stored ? add_s * 1e6 / stored : 0.0,
		stored ? interp_s * 1e6 / stored : 0.0,
		stored ? bezier_s * 1e6 / stored : 0.0,
		stored ? tan_s * 1e6 / stored : 0.0);

	S_last_result_len	= (long)stored;
	S_last_ms			= (long)((add_s + interp_s + tan_s
									+ (do_bezier ? bezier_s : 0.0)) * 1000.0);
	PipeWriteLine(msg);
}

// ---------------------------------------------------------------------------
//	pipe server  (background thread)
// ---------------------------------------------------------------------------

static void
HandleLine(const char *lineZ)
{
	BridgeRequest r;

	ZeroMemory(&r, sizeof(r));

	if (!JsonStr(lineZ, "\"cmd\":", r.cmd, sizeof(r.cmd))) {
		Log("  rx: no cmd in %.120s\n", lineZ);
		return;
	}
	JsonStr(lineZ, "\"script\":", r.arg, sizeof(r.arg));
	JsonStr(lineZ, "\"bytes\":", r.num, sizeof(r.num));
	JsonStr(lineZ, "\"mode\":", r.mode, sizeof(r.mode));

	//	Only pipe_probe carries one, and only it pays for the allocation.
	if (!strcmp(r.cmd, "pipe_probe")) {
		r.dataP = JsonStrAlloc(lineZ, "\"data\":", &r.data_len);
	}

	//	Sent as a quoted string like every other field, so the reader stays
	//	the one that only knows how to pull a quoted value.
	char echoZ[8] = { 0 };

	if (JsonStr(lineZ, "\"echo\":", echoZ, sizeof(echoZ))) {
		r.echo = (echoZ[0] == '1' || echoZ[0] == 't') ? TRUE : FALSE;
	}
	if (JsonStr(lineZ, "\"purge\":", echoZ, sizeof(echoZ))) {
		r.purge = (echoZ[0] == '1' || echoZ[0] == 't') ? TRUE : FALSE;
	}
	Log("  rx: cmd=%s arg=%s bytes=%s mode=%s echo=%d data=%llu\n", r.cmd,
		r.arg, r.num, r.mode, (int)r.echo, (unsigned long long)r.data_len);

	if (!QueuePush(&r) && r.dataP) {
		free(r.dataP);			//	refused, so it never moved -- see the struct
	}
}

static DWORD WINAPI
PipeServerThread(LPVOID)
{
	for (;;) {
		//	Both pipes are created before either is connected, so a client can
		//	open them back to back. TX is outbound and is the handle the UI
		//	thread writes to; RX is inbound and is the one this thread parks on.
		//	Neither ever carries both directions, so a UI-thread write can never
		//	queue behind this thread's blocking read.
		HANDLE tx = CreateNamedPipeA(
			PHYSBRIDGE_PIPE_TX,
			PIPE_ACCESS_OUTBOUND,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1, 1 << 16, 1 << 16, 0, NULL);

		HANDLE rx = CreateNamedPipeA(
			PHYSBRIDGE_PIPE_RX,
			PIPE_ACCESS_INBOUND,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1, 1 << 16, 1 << 16, 0, NULL);

		if (tx == INVALID_HANDLE_VALUE || rx == INVALID_HANDLE_VALUE) {
			Log("  pipe: CreateNamedPipe failed, err=%lu\n",
				(unsigned long)GetLastError());
			if (tx != INVALID_HANDLE_VALUE) CloseHandle(tx);
			if (rx != INVALID_HANDLE_VALUE) CloseHandle(rx);
			return 1;
		}

		BOOL tx_ok = ConnectNamedPipe(tx, NULL)
						? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
		BOOL rx_ok = tx_ok && (ConnectNamedPipe(rx, NULL)
						? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED));

		if (WaitForSingleObject(S_pipe_stop_evt, 0) == WAIT_OBJECT_0) {
			CloseHandle(tx);
			CloseHandle(rx);
			return 0;
		}
		if (!tx_ok || !rx_ok) {
			CloseHandle(tx);
			CloseHandle(rx);
			continue;
		}

		EnterCriticalSection(&S_cs);
		S_pipe_tx	= tx;
		S_pipe_rx	= rx;
		S_connected	= TRUE;
		LeaveCriticalSection(&S_cs);
		Log("  pipe: client connected\n");

		{
			char	buf[4096];
			size_t	acc_cap	= PHYSBRIDGE_LINE_START;
			size_t	acc_len	= 0;
			char	*accP	= (char *)malloc(acc_cap);
			A_Boolean over	= FALSE;

			for (; accP; ) {
				DWORD got = 0;

				if (!ReadFile(rx, buf, sizeof(buf), &got, NULL) || !got) {
					break;
				}
				for (DWORD i = 0; i < got; i++) {
					char c = buf[i];

					if (c == '\n') {
						if (over) {
							//	Say so on the wire. A request that disappears
							//	in silence gets diagnosed as a hung bridge.
							BridgeRequest r;

							ZeroMemory(&r, sizeof(r));
							strcpy_s(r.cmd, sizeof(r.cmd), "_overflow");
							QueuePush(&r);
							over = FALSE;
						} else if (acc_len) {
							accP[acc_len] = 0;
							HandleLine(accP);
						}
						acc_len = 0;
						continue;
					}
					if (over) {
						continue;		//	skip to the end of the bad line
					}
					if (acc_len + 2 > acc_cap) {
						size_t want = acc_cap * 2;

						if (want > PHYSBRIDGE_LINE_CAP) {
							Log("  rx: request over %llu bytes, refused\n",
								(unsigned long long)PHYSBRIDGE_LINE_CAP);
							over = TRUE;
							acc_len = 0;
							continue;
						}
						char *bigP = (char *)realloc(accP, want);

						if (!bigP) {
							Log("  rx: out of memory growing to %llu\n",
								(unsigned long long)want);
							over = TRUE;
							acc_len = 0;
							continue;
						}
						accP	= bigP;
						acc_cap	= want;
					}
					accP[acc_len++] = c;
				}
			}
			free(accP);
		}

		Log("  pipe: client gone\n");

		EnterCriticalSection(&S_cs);
		S_pipe_tx	= INVALID_HANDLE_VALUE;
		S_pipe_rx	= INVALID_HANDLE_VALUE;
		S_connected	= FALSE;
		LeaveCriticalSection(&S_cs);

		DisconnectNamedPipe(tx);
		DisconnectNamedPipe(rx);
		CloseHandle(tx);
		CloseHandle(rx);

		if (WaitForSingleObject(S_pipe_stop_evt, 0) == WAIT_OBJECT_0) {
			return 0;
		}
	}
}

static void
StartPipeServer(void)
{
	S_pipe_stop_evt = CreateEventA(NULL, TRUE, FALSE, NULL);
	S_pipe_thread	= CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);

	if (!S_pipe_thread) {
		Log("  pipe: CreateThread failed, err=%lu\n",
			(unsigned long)GetLastError());
	} else {
		Log("  pipe: server listening on %s / %s\n",
			PHYSBRIDGE_PIPE_TX, PHYSBRIDGE_PIPE_RX);
	}
}

static void
StopPipeServer(void)
{
	if (S_pipe_stop_evt) {
		SetEvent(S_pipe_stop_evt);
	}

	//	Poke both pipes so the thread leaves its blocking ConnectNamedPipe.
	HANDLE h = CreateFileA(PHYSBRIDGE_PIPE_TX, GENERIC_READ, 0, NULL,
							OPEN_EXISTING, 0, NULL);
	if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

	h = CreateFileA(PHYSBRIDGE_PIPE_RX, GENERIC_WRITE, 0, NULL,
					OPEN_EXISTING, 0, NULL);
	if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

	if (S_pipe_thread) {
		WaitForSingleObject(S_pipe_thread, 2000);
		CloseHandle(S_pipe_thread);
		S_pipe_thread = NULL;
	}
	if (S_pipe_stop_evt) {
		CloseHandle(S_pipe_stop_evt);
		S_pipe_stop_evt = NULL;
	}
}

// ---------------------------------------------------------------------------
//	reading a bake  (a document, not a protocol line)
// ---------------------------------------------------------------------------

/*	JsonStr above is the right size for the PROTOCOL: one flat object, string
	values, no nesting. A bake is a different kind of document -- nested
	objects, arrays of arrays, thousands of numbers -- and `strstr` for a key
	name is actively dangerous in it, because "name" occurs inside the source
	block, inside every layer, and inside any comp a user called "name".

	So this is a second reader, structural rather than textual: it walks
	values, it knows where one ends, and a lookup only ever searches the keys
	of ONE object. It is still deliberately small -- no DOM, no allocation. It
	returns POINTERS INTO the buffer, so the buffer must outlive every pointer
	taken from it.

	It does not decode \u escapes, for the same reason JsonStr does not: the
	two readers would then disagree about the same wire format, and B1's writer
	emits neither. */

static const char *
JsonWs(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
		p++;
	}
	return p;
}

/*	The end of the value starting at `p`, one past its last character.

	Strings are skipped as strings -- with their escapes -- BEFORE any bracket
	counting, which is the whole reason this is a function and not a loop with
	a depth counter. A layer named "]" would otherwise close an array that is
	still open, and a bake whose layer names came from a real project is
	exactly where that happens. */
static const char *
JsonEnd(const char *p)
{
	p = JsonWs(p);

	if (*p == '"') {
		p++;
		while (*p && *p != '"') {
			p += (*p == '\\' && p[1]) ? 2 : 1;
		}
		return *p ? p + 1 : p;
	}
	if (*p == '{' || *p == '[') {
		char	open	= *p;
		char	close	= (open == '{') ? '}' : ']';
		int		depth	= 0;

		while (*p) {
			if (*p == '"') {
				p++;
				while (*p && *p != '"') {
					p += (*p == '\\' && p[1]) ? 2 : 1;
				}
				if (!*p) {
					break;
				}
			} else if (*p == open) {
				depth++;
			} else if (*p == close) {
				depth--;
				if (!depth) {
					return p + 1;
				}
			}
			p++;
		}
		return p;
	}
	while (*p && *p != ',' && *p != '}' && *p != ']'
			&& *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
		p++;
	}
	return p;
}

//	The value of `nameZ` in the object at `objP`, or NULL. Searches the keys of
//	that object only -- it never descends, which is the point.
static const char *
JsonMember(const char *objP, const char *nameZ)
{
	if (!objP) {
		return NULL;
	}
	const char	*p		= JsonWs(objP);
	size_t		want	= strlen(nameZ);

	if (*p != '{') {
		return NULL;
	}
	p++;

	while (*p) {
		p = JsonWs(p);

		if (*p == '}') {
			return NULL;
		}
		if (*p != '"') {
			return NULL;			//	malformed; refuse rather than guess
		}
		const char *k = p + 1;
		const char *ke = JsonEnd(p);		//	one past the closing quote

		p = JsonWs(ke);
		if (*p != ':') {
			return NULL;
		}
		p = JsonWs(p + 1);

		A_Boolean hit = ((size_t)(ke - 1 - k) == want)
						&& !strncmp(k, nameZ, want);

		if (hit) {
			return p;
		}
		p = JsonWs(JsonEnd(p));
		if (*p == ',') {
			p++;
		}
	}
	return NULL;
}

//	Element `idx` of the array at `arrP`, or NULL.
static const char *
JsonElem(const char *arrP, long idx)
{
	if (!arrP) {
		return NULL;
	}
	const char *p = JsonWs(arrP);

	if (*p != '[') {
		return NULL;
	}
	p = JsonWs(p + 1);

	for (long i = 0; *p && *p != ']'; i++) {
		if (i == idx) {
			return p;
		}
		p = JsonWs(JsonEnd(p));
		if (*p == ',') {
			p = JsonWs(p + 1);
		}
	}
	return NULL;
}

static long
JsonCount(const char *arrP)
{
	if (!arrP) {
		return -1;
	}
	const char *p = JsonWs(arrP);

	if (*p != '[') {
		return -1;
	}
	p = JsonWs(p + 1);

	long n = 0;

	while (*p && *p != ']') {
		n++;
		p = JsonWs(JsonEnd(p));
		if (*p == ',') {
			p = JsonWs(p + 1);
		}
	}
	return n;
}

/*	A number, reported as "was there one" separately from its value.

	strtod returning 0.0 is indistinguishable from a real 0.0, and a bake is
	full of real zeroes -- a rotation of 0, a position at the origin. A missing
	field silently becoming 0.0 would place a layer at the top-left corner and
	look like physics. */
static A_Boolean
JsonNum(const char *p, double *outP)
{
	if (!p) {
		return FALSE;
	}
	p = JsonWs(p);

	char *endP = NULL;
	double v = strtod(p, &endP);

	if (endP == p) {
		return FALSE;
	}
	*outP = v;
	return TRUE;
}

static A_Boolean
JsonIsTrue(const char *p)
{
	return (p && !strncmp(JsonWs(p), "true", 4)) ? TRUE : FALSE;
}

static A_Boolean
JsonText(const char *p, char *outZ, size_t out_max)
{
	if (!p) {
		return FALSE;
	}
	p = JsonWs(p);

	if (*p != '"') {
		return FALSE;
	}
	p++;

	size_t o = 0;

	while (*p && *p != '"' && o + 1 < out_max) {
		if (*p == '\\' && p[1]) {
			p++;
			outZ[o++] = (*p == 'n') ? '\n' : *p;
			p++;
		} else {
			outZ[o++] = *p++;
		}
	}
	outZ[o] = 0;
	return TRUE;
}

// ---------------------------------------------------------------------------
//	applying a bake  (C0.3's measurement, wired into the product)
// ---------------------------------------------------------------------------

/*	C0.3 measured the native keyframe path and then left it sitting unused
	while the product paid ExtendScript's price. This is the wiring.

	    the LINEAR pass, per key      ExtendScript 853 us    native 88.5
	    the whole apply, per key                   872.6            117.7

	about 7.4x -- a smaller and more honest number than the 10x on the pass
	alone, because native's batch add is SLOWER than setValuesAtTimes (29-31
	us/key against 19.6). The entire win is the interpolation pass. B2 had
	already established that writing values is free; what costs is making them
	mean what we meant.

	WHAT THIS DOES NOT DO, AND WHY THE .JSX STAYS.

	b2_apply_bake.jsx is not deleted and not deprecated. It is the reference
	implementation: every number above is a comparison against it, and a second
	implementation you can no longer run is a second implementation you can no
	longer check. This command exists beside it.

	THE PHASE THAT IS DELIBERATELY MISSING.

	C0.3 ran four phases and found phase 3 -- clearing SPATIAL_AUTOBEZIER per
	key -- costs more than everything else together AND grows per key with the
	key count: 167 us/key at 1,000 and 2,765 at 12,000, which is O(n^2) overall.
	Something inside it walks the keyframe list every time. Skipping it left
	the motion path straight, so it is skipped here.

	But C0.3 was explicit that it measured THAT and assumed WHY. Two
	explanations fit the same evidence: AEGP_SetKeyframeSpatialTangents may
	clear the flag as a side effect, or keyframes created through
	AEGP_AddKeyframes may never get spatial auto-bezier in the first place. The
	read-back happened after the tangent phase, so it could not tell them apart.

	It does not matter for correctness -- either way the path comes out
	straight -- and it matters enormously for TRUST, because the product now
	depends on it. So this reads the flag back on a sample and REPORTS it. If
	AE ever stops behaving the way C0.3 measured, the apply says so in its
	reply instead of quietly bowing every motion path between keyframes, which
	is A5's invisible failure and the one nobody can see on a still. */

typedef struct {
	long	layers;
	long	keys;
	long	skipped_static;
	double	add_s;
	double	interp_s;
	double	tan_s;
	long	autobezier_still_set;	//	-1 = not sampled
	long	sampled;
} ApplyTally;

/*	One stream, from the bake's frame/value arrays.

	`spatial` is FALSE for Rotation, and not as an optimisation: rotation is
	not a spatial property and has no tangents to zero. B2 found the arity
	belongs to the CALL rather than the property -- a 2D Position takes a
	2-element value and a 3-element tangent -- so the value is written through
	three_d regardless and the tangent pass is what varies. */
static A_Err
ApplyStream(AEGP_SuiteHandler	&suites,
			AEGP_StreamRefH		streamH,
			const char			*arrP,		//	[[frame, value], ...]
			A_Boolean			spatial,
			double				fps,
			ApplyTally			*tallyP)
{
	A_Err	err = A_Err_NONE, err2 = A_Err_NONE;
	long	n	= JsonCount(arrP);

	if (n <= 0) {
		return A_Err_NONE;			//	no keys is a legitimate answer
	}

	AEGP_AddKeyframesInfoH	akH = NULL;
	double					t0	= NowSeconds();

	ERR(suites.KeyframeSuite4()->AEGP_StartAddKeyframes(streamH, &akH));

	for (long i = 0; !err && i < n; i++) {
		const char	*kP		= JsonElem(arrP, i);
		const char	*fP		= JsonElem(kP, 0);
		const char	*vP		= JsonElem(kP, 1);
		double		frame	= 0.0;

		if (!JsonNum(fP, &frame)) {
			err = A_Err_PARAMETER;
			break;
		}

		/*	The time is built from the frame number and the comp's fps as a
			RATIONAL, never as seconds. A_Time is a fraction for a reason: at
			23.976 the seconds form lands between frames and AE snaps it to
			whichever side rounding picked, which is how a bake acquires a
			one-frame stutter that nothing in the numbers explains. */
		A_Time	t		= { (A_long)(frame + 0.5), (A_u_long)(fps + 0.5) };
		A_long	index	= 0;

		ERR(suites.KeyframeSuite4()->AEGP_AddKeyframes(akH,
				AEGP_LTimeMode_LayerTime, &t, &index));

		AEGP_StreamValue2 v;

		AEFX_CLR_STRUCT(v);
		v.streamH = streamH;

		if (JsonCount(vP) >= 2) {				//	position: [x, y]
			double x = 0.0, y = 0.0;

			if (!JsonNum(JsonElem(vP, 0), &x)
					|| !JsonNum(JsonElem(vP, 1), &y)) {
				err = A_Err_PARAMETER;
				break;
			}
			v.val.three_d.x = x;
			v.val.three_d.y = y;
			v.val.three_d.z = 0.0;
		} else {								//	rotation: a bare number
			double a = 0.0;

			if (!JsonNum(vP, &a)) {
				err = A_Err_PARAMETER;
				break;
			}
			v.val.one_d = a;
		}
		ERR(suites.KeyframeSuite4()->AEGP_SetAddKeyframe(akH, index, &v));
	}
	ERR(suites.KeyframeSuite4()->AEGP_EndAddKeyframes(TRUE, akH));

	tallyP->add_s += NowSeconds() - t0;

	if (err) {
		return err;
	}

	//	Did they all land? Writing fewer keys than the bake holds is a silent
	//	partial apply, which is worse than a failure.
	A_long stored = 0;

	ERR2(suites.KeyframeSuite4()->AEGP_GetStreamNumKFs(streamH, &stored));
	tallyP->keys += stored;

	// -- the pass Wall I is actually about ---------------------------------
	t0 = NowSeconds();

	for (A_long i = 0; !err && i < stored; i++) {
		ERR(suites.KeyframeSuite4()->AEGP_SetKeyframeInterpolation(streamH, i,
				AEGP_KeyInterp_LINEAR, AEGP_KeyInterp_LINEAR));
	}
	tallyP->interp_s += NowSeconds() - t0;

	if (!spatial || err) {
		return err;
	}

	// -- zero the spatial tangents -----------------------------------------
	AEGP_StreamValue2 zero;

	AEFX_CLR_STRUCT(zero);
	zero.streamH = streamH;

	t0 = NowSeconds();

	for (A_long i = 0; !err && i < stored; i++) {
		ERR(suites.KeyframeSuite4()->AEGP_SetKeyframeSpatialTangents(streamH,
				i, &zero, &zero));
	}
	tallyP->tan_s += NowSeconds() - t0;

	/*	The assumption, read back rather than trusted.

		Sampled rather than exhaustive, and for a specific reason: the flag pass
		this design SKIPS is the O(n^2) one, so reading every key back would
		reintroduce the very cost being dodged. The stride is C0.3's -- a spread
		of keys with the last one always included, because an endpoint is where AE
		is most likely to treat a keyframe differently and the cheapest place for
		this to be wrong unnoticed.

		It counts rather than aborts. The keys are already written and correct in
		value; a set flag means the PATH BETWEEN them bows, which the caller
		should be told about plainly rather than have the apply rolled back
		underneath it. */
	const long	WANT = 32;
	A_long		step = stored / WANT;

	if (step < 1) {
		step = 1;
	}
	if (tallyP->autobezier_still_set < 0) {
		tallyP->autobezier_still_set = 0;
	}
	for (A_long i = 0; !err2 && i < stored; i += step) {
		A_long				k		= (i + step >= stored) ? (stored - 1) : i;
		AEGP_KeyframeFlags	flags	= AEGP_KeyframeFlag_NONE;

		ERR2(suites.KeyframeSuite4()->AEGP_GetKeyframeFlags(streamH, k, &flags));

		if (flags & AEGP_KeyframeFlag_SPATIAL_AUTOBEZIER) {
			tallyP->autobezier_still_set++;
		}
		tallyP->sampled++;
	}
	return err;
}

/*	A layer's name as UTF-8, which is what the bake is written in.

	AEGP_GetLayerName hands back UTF-16 in AEGP_MemHandles the caller must lock
	and dispose -- not the A_char buffers the older suites used. Getting that
	wrong is a leak per layer, on a path that runs once for every apply.

	The SOURCE name is the fallback, and not as defensive padding: it is what
	AE displays for a layer the user has never renamed, and what
	b1_read_shapes.jsx recorded for it. Comparing a display name against an
	empty override would fail on every untouched layer in the comp. */
static void
LayerNameUTF8(AEGP_SuiteHandler	&suites,
			  AEGP_LayerH		layerH,
			  char				*outZ,
			  size_t			out_max)
{
	//	ERR2 writes into BOTH; err is a local sink here on purpose. A name
	//	that could not be read comes back empty and fails the comparison
	//	loudly, which is the outcome we want anyway.
	A_Err			err	= A_Err_NONE,
					err2	= A_Err_NONE;
	AEGP_MemHandle	nameH	= NULL,
					srcH	= NULL;

	outZ[0] = 0;

	ERR2(suites.LayerSuite8()->AEGP_GetLayerName(S_my_id, layerH,
			&nameH, &srcH));

	for (int pass = 0; pass < 2; pass++) {
		AEGP_MemHandle	h = pass ? srcH : nameH;
		A_UTF16Char		*uP = NULL;

		if (!h || outZ[0]) {
			continue;
		}
		ERR2(suites.MemorySuite1()->AEGP_LockMemHandle(h, (void **)&uP));

		if (uP) {
			WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)uP, -1,
				outZ, (int)out_max, NULL, NULL);
			ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(h));
		}
	}
	if (nameH) {
		ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(nameH));
	}
	if (srcH) {
		ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(srcH));
	}
}

//	{"cmd":"apply_bake","script":"<absolute path to bake.json>"}
static void
DoApplyBake(AEGP_SuiteHandler &suites, const BridgeRequest *rP)
{
	A_Err	err = A_Err_NONE, err2 = A_Err_NONE;
	char	msg[768];

	// -- the document ------------------------------------------------------
	FILE *fP = NULL;

	if (fopen_s(&fP, rP->arg, "rb") || !fP) {
		sprintf_s(msg, sizeof(msg), "cannot open the bake at '%s'", rP->arg);
		PipeWriteError(msg);
		return;
	}
	fseek(fP, 0, SEEK_END);
	long len = ftell(fP);
	fseek(fP, 0, SEEK_SET);

	if (len <= 0) {
		fclose(fP);
		PipeWriteError("the bake file is empty");
		return;
	}
	char *docP = (char *)malloc((size_t)len + 1);

	if (!docP) {
		fclose(fP);
		PipeWriteError("out of memory reading the bake");
		return;
	}
	size_t got = fread(docP, 1, (size_t)len, fP);

	fclose(fP);
	docP[got] = 0;

	const char *layersP = JsonMember(docP, "layers");
	long		nlayers = JsonCount(layersP);

	if (nlayers <= 0) {
		free(docP);
		PipeWriteError("this bake has no layers array -- it is not an "
						"ae-physics-bake document");
		return;
	}

	// -- the comp ----------------------------------------------------------
	AEGP_CompH	compH = NULL;

	ERR(suites.CompSuite11()->AEGP_GetMostRecentlyUsedComp(&compH));

	if (err || !compH) {
		free(docP);
		PipeWriteError("no comp is open -- open the comp this bake was made "
						"from and try again");
		return;
	}

	A_long		nlayers_ae	= 0;
	A_FpLong	fps			= 0.0;
	A_Time		frame_dur	= { 0, 1 };

	ERR2(suites.LayerSuite8()->AEGP_GetCompNumLayers(compH, &nlayers_ae));
	ERR2(suites.CompSuite11()->AEGP_GetCompFramerate(compH, &fps));

	if (fps <= 0.0) {
		free(docP);
		PipeWriteError("the comp reports a frame rate of zero");
		return;
	}

	/*	Wall K, the half that lives in AE.

		The SHELL already re-reads the comp and compares scene hashes, which is
		the only check that can see a moved layer -- see verify.rs. This is not
		that check and does not replace it. It is the .jsx's tripwire, kept
		because this command can also be driven by something that never asked
		the shell: layer index is POSITIONAL, and applying a bake to a comp
		whose layers were reordered writes good keyframes onto the wrong
		objects. Cheap, and it refuses rather than warns, because there is no
		reading of "the layer at index 3 is now called something else" that
		makes writing to it correct. */
	for (long i = 0; i < nlayers; i++) {
		const char	*L = JsonElem(layersP, i);
		double		id = 0.0;
		char		want[128] = { 0 }, have[128] = { 0 };

		if (!JsonNum(JsonMember(L, "id"), &id)) {
			continue;
		}
		if ((A_long)id < 1 || (A_long)id > nlayers_ae) {
			sprintf_s(msg, sizeof(msg),
					"id %d (%s) is outside this comp, which has %d layers -- "
					"nothing was written",
					(int)id, want, (int)nlayers_ae);
			free(docP);
			PipeWriteError(msg);
			return;
		}
		JsonText(JsonMember(L, "name"), want, sizeof(want));

		AEGP_LayerH	layerH = NULL;

		ERR2(suites.LayerSuite8()->AEGP_GetCompLayerByIndex(compH,
				(A_long)id - 1, &layerH));

		if (layerH) {
			LayerNameUTF8(suites, layerH, have, sizeof(have));
		}
		if (strcmp(want, have)) {
			sprintf_s(msg, sizeof(msg),
					"id %d: the bake says '%s', this comp has '%s' -- nothing "
					"was written. Layer index is positional; re-read the comp "
					"if layers were added, deleted or reordered.",
					(int)id, want, have);
			free(docP);
			PipeWriteError(msg);
			return;
		}
	}

	// -- write -------------------------------------------------------------
	ApplyTally	tally;
	double		wall0 = NowSeconds();

	memset(&tally, 0, sizeof(tally));
	tally.autobezier_still_set = -1;

	/*	ONE undo group for the whole apply. B2 found that a throw between
		beginUndoGroup and endUndoGroup leaves AE wedged with a half apply, so
		every exit below this point goes through the same end -- there is no
		early return between here and it. */
	ERR2(suites.UtilitySuite3()->AEGP_StartUndoGroup("Apply physics bake"));

	for (long i = 0; !err && i < nlayers; i++) {
		const char	*L	= JsonElem(layersP, i);
		double		id	= 0.0;

		if (!JsonNum(JsonMember(L, "id"), &id)) {
			continue;
		}

		/*	B3: a pinned layer gets NO keyframes at all, not a constant one.
			Writing even a single unchanging key would overwrite the user's own
			placement and silently undo any later nudge. */
		if (JsonIsTrue(JsonMember(L, "static"))) {
			tally.skipped_static++;
			continue;
		}

		const char *kfP = JsonMember(L, "keyframes");
		AEGP_LayerH	layerH = NULL;

		ERR(suites.LayerSuite8()->AEGP_GetCompLayerByIndex(compH,
				(A_long)id - 1, &layerH));

		AEGP_StreamRefH	posH = NULL, rotH = NULL;

		ERR(suites.StreamSuite2()->AEGP_GetNewLayerStream(S_my_id, layerH,
				AEGP_LayerStream_POSITION, &posH));

		if (!err && posH) {
			err = ApplyStream(suites, posH, JsonMember(kfP, "position"),
								TRUE, (double)fps, &tally);
			ERR2(suites.StreamSuite2()->AEGP_DisposeStream(posH));
		}

		/*	Rotation, not Z_ROTATION: a 2D layer's rotation is the one-
			dimensional ROTATION stream, and it has no spatial tangents to
			zero -- which is also why B2's 853 us/key is a BLEND that flatters
			ExtendScript. Its makeLinear() makes three calls per Position key
			and one per Rotation key, and the cheap rotation keys pull the
			average down. */
		if (!err) {
			ERR(suites.StreamSuite2()->AEGP_GetNewLayerStream(S_my_id, layerH,
					AEGP_LayerStream_ROTATION, &rotH));

			if (!err && rotH) {
				err = ApplyStream(suites, rotH, JsonMember(kfP, "rotation"),
									FALSE, (double)fps, &tally);
				ERR2(suites.StreamSuite2()->AEGP_DisposeStream(rotH));
			}
		}
		if (!err) {
			tally.layers++;
		}
	}

	ERR2(suites.UtilitySuite3()->AEGP_EndUndoGroup());

	double wall_s = NowSeconds() - wall0;

	free(docP);

	if (err) {
		PipeWriteError("the apply failed partway through -- one Undo puts the "
						"comp back the way it was");
		return;
	}

	double per_key = tally.keys ? (wall_s * 1e6 / (double)tally.keys) : 0.0;

	sprintf_s(msg, sizeof(msg),
			"{\"ok\":true,\"layers\":%ld,\"keys\":%ld,\"skipped_static\":%ld,"
			"\"ms\":%.1f,\"us_per_key\":%.1f,\"add_ms\":%.1f,"
			"\"interp_ms\":%.1f,\"tangents_ms\":%.1f,"
			"\"autobezier_sampled\":%ld,\"autobezier_still_set\":%ld}",
			tally.layers, tally.keys, tally.skipped_static,
			wall_s * 1000.0, per_key, tally.add_s * 1000.0,
			tally.interp_s * 1000.0, tally.tan_s * 1000.0,
			tally.sampled, tally.autobezier_still_set);
	PipeWriteLine(msg);

	Log("  apply_bake: %ld layers, %ld keys, %.1f ms (%.1f us/key)\n",
		tally.layers, tally.keys, wall_s * 1000.0, per_key);
}

/*	A string ON THE WAY OUT, escaped so the reply is JSON.

	Every reply this plug-in has written until now has been numbers, fixed
	words, or a document AE itself produced -- so nothing here has ever had to
	escape anything, and there is no helper for it. `comp_identity` is the
	first reply carrying a WINDOWS PATH, and a Windows path is backslashes:
	`C:\Work\p.aep` written raw produces `"C:\W..."`, where \W is not a legal
	escape. The reply would not parse, and the app's identity call would fail
	in the one way that looks like "the plug-in is too old".

	Deliberately narrow, exactly as JsonStr coming the other way is: the two
	characters JSON requires plus the control range, and no \u handling beyond
	it. What arrives here is already UTF-8 from WideCharToMultiByte and passes
	through unexamined. Truncation is silent and safe -- the output is always
	terminated and always balanced, because nothing is ever cut mid-escape. */
static void
JsonOutStr(const char *inZ, char *outZ, size_t out_max)
{
	size_t	o = 0;

	if (!out_max) {
		return;
	}
	for (const unsigned char *p = (const unsigned char *)inZ; p && *p; p++) {
		char	esc[8];
		size_t	n;

		switch (*p) {
			case '\\':	strcpy_s(esc, sizeof(esc), "\\\\");	break;
			case '"':	strcpy_s(esc, sizeof(esc), "\\\"");	break;
			case '\n':	strcpy_s(esc, sizeof(esc), "\\n");	break;
			case '\r':	strcpy_s(esc, sizeof(esc), "\\r");	break;
			case '\t':	strcpy_s(esc, sizeof(esc), "\\t");	break;
			default:
				if (*p < 0x20) {
					sprintf_s(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
				} else {
					esc[0] = (char)*p;
					esc[1] = 0;
				}
		}
		n = strlen(esc);

		if (o + n + 1 >= out_max) {
			break;		//	never cut an escape in half
		}
		memcpy(outZ + o, esc, n);
		o += n;
	}
	outZ[o] = 0;
}

/*	A UTF-16 AEGP_MemHandle as UTF-8, locked, converted, unlocked, freed.

	The same sequence LayerNameUTF8 does, without the name-then-source
	fallback, which is a layer-name rule and means nothing for a project path.
	Getting the dispose wrong is a leak per call. */
static void
MemHandleUTF8(AEGP_SuiteHandler	&suites,
			  AEGP_MemHandle	h,
			  char				*outZ,
			  size_t			out_max)
{
	A_Err			err		= A_Err_NONE,
					err2	= A_Err_NONE;
	A_UTF16Char		*uP		= NULL;

	outZ[0] = 0;

	if (!h) {
		return;
	}
	ERR2(suites.MemorySuite1()->AEGP_LockMemHandle(h, (void **)&uP));

	if (uP) {
		WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)uP, -1,
			outZ, (int)out_max, NULL, NULL);
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(h));
	}
	ERR2(suites.MemorySuite1()->AEGP_FreeMemHandle(h));
}

/*	C7.1 -- which comp is this, and which project is it in?

	WHY THE ANSWER IS NOT IN THE SCENE DOCUMENT
	-------------------------------------------
	The obvious place for a comp id is the scene's `comp` block, next to name,
	width and fps. It is the wrong place, and the roadmap names why: the shell
	hashes the WHOLE raw scene document for Wall K (verify.rs), so anything
	added to it changes the hash. A comp id would survive that -- it does not
	move -- but a PROJECT PATH does: Save As between simulate and apply would
	change the path, change the hash, and make the staleness guard refuse a
	bake that is still physically valid. A guard that fires on a correct bake
	is a guard people learn to click through.

	So identity travels as its own request. It is outside the hashed bytes by
	construction rather than by remembering to keep it out, `b1_read_shapes.jsx`
	is not touched, and every scene document captured before today still hashes
	to what it hashed before -- which matters because B1's fixture and the
	September export are evidence, not just files.

	THE COMP IS THE ONE `apply_bake` ALREADY USES. `AEGP_GetMostRecentlyUsedComp`
	rather than the reader's `app.project.activeItem`: those can differ, but
	DoApplyBake already pairs this call with a document produced by the reader,
	so using a different one HERE would put a third opinion about "the comp" in
	a system that currently has two. If that pairing is ever wrong it is wrong
	for the apply first, which is where it would be found and fixed.

	An unsaved project has no path, and says so rather than sending something
	that looks like one. The app keys such a setup more weakly and tells the
	person; see `CompIdentity::key`. */
static void
DoCompIdentity(AEGP_SuiteHandler &suites)
{
	A_Err		err = A_Err_NONE, err2 = A_Err_NONE;
	AEGP_CompH	compH = NULL;

	ERR(suites.CompSuite11()->AEGP_GetMostRecentlyUsedComp(&compH));

	if (err || !compH) {
		PipeWriteError("no comp is open, so there is nothing to identify");
		return;
	}

	AEGP_ItemH	itemH	= NULL;
	A_long		item_id	= 0;
	char		name[512] = { 0 };

	ERR2(suites.CompSuite11()->AEGP_GetItemFromComp(compH, &itemH));

	if (itemH) {
		AEGP_MemHandle	nameH = NULL;

		ERR2(suites.ItemSuite9()->AEGP_GetItemID(itemH, &item_id));
		ERR2(suites.ItemSuite9()->AEGP_GetItemName(S_my_id, itemH, &nameH));
		MemHandleUTF8(suites, nameH, name, sizeof(name));
	}

	/*	item_id 0 is not a comp whose id happens to be zero -- AEGP item ids
		start at 1 -- it is "the call did not answer". Reported as an id of 0
		rather than as an error, because the app already has a state for an
		unidentified comp and it is a better one than a failed request: the
		parameters on screen simply belong to nothing in particular, and it
		says so. */

	AEGP_ProjectH	projH = NULL;
	char			path[1024] = { 0 },
					proj[256] = { 0 };	//	AEGP_MAX_PROJ_NAME_SIZE is 48

	ERR2(suites.ProjSuite6()->AEGP_GetProjectByIndex(0, &projH));

	if (projH) {
		AEGP_MemHandle	pathH = NULL;

		ERR2(suites.ProjSuite6()->AEGP_GetProjectName(projH, proj));
		ERR2(suites.ProjSuite6()->AEGP_GetProjectPath(projH, &pathH));
		MemHandleUTF8(suites, pathH, path, sizeof(path));
	}

	//	Escaped into their own buffers FIRST: a path is backslashes, and one
	//	of them a character short of the end is how a reply stops being JSON.
	char	name_esc[1200] = { 0 },
			path_esc[2400] = { 0 },
			proj_esc[600]  = { 0 },
			msg[4600];

	JsonOutStr(name, name_esc, sizeof(name_esc));
	JsonOutStr(path, path_esc, sizeof(path_esc));
	JsonOutStr(proj, proj_esc, sizeof(proj_esc));

	sprintf_s(msg, sizeof(msg),
			"{\"ok\":true,\"comp_id\":%ld,\"comp_name\":\"%s\","
			"\"project_path\":\"%s\",\"project_name\":\"%s\","
			"\"saved\":%s}",
			(long)item_id, name_esc, path_esc, proj_esc,
			path[0] ? "true" : "false");
	Log("  comp_identity: id %ld '%s' in '%s'\n", (long)item_id, name,
			path[0] ? path : "(unsaved)");
	PipeWriteLine(msg);
}

/*	Bring After Effects to the front.

	The hand-off at the end of an apply: the keyframes are in the project, so the
	place to look is AE, not the window that just wrote them. The app minimises
	itself and asks for this.

	WHY THE PLUG-IN DOES IT RATHER THAN THE APP.

	The app could call SetForegroundWindow with AE's HWND -- it is the foreground
	process at that moment, so Windows would allow it. But that means teaching the
	Tauri side about window handles and adding a Win32 dependency to a program
	that otherwise only moves bytes. The plug-in is already inside AE and already
	has the headers, and AEGP_GetMainHWND hands it the window directly.

	WHAT IS NOT PROMISED.

	SetForegroundWindow is advisory. Windows refuses it from a process that is not
	foreground and has not received recent input, which is exactly this plug-in's
	situation -- so the app minimising itself is what actually reveals AE, and
	this is the part that raises rather than merely uncovers it. If it is refused,
	nothing breaks and AE is simply behind where the app used to be. The reply
	says which happened rather than claiming success either way. */
static void
DoFocusAE(AEGP_SuiteHandler &suites)
{
	A_Err	err2 = A_Err_NONE, err = A_Err_NONE;
	HWND	hwnd = NULL;

	ERR2(suites.UtilitySuite6()->AEGP_GetMainHWND(&hwnd));

	if (!hwnd) {
		PipeWriteError("After Effects did not hand back its main window");
		return;
	}

	//	A minimised AE has to be restored first, or it is brought to the front
	//	as an icon and the user is told the app closed for nothing.
	if (IsIconic(hwnd)) {
		ShowWindow(hwnd, SW_RESTORE);
	}

	BOOL	ok = SetForegroundWindow(hwnd);

	if (!ok) {
		//	Second try, which is the one that usually works: bring the window to
		//	the top of the Z-order even when it cannot take input focus.
		SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}

	char msg[96];

	sprintf_s(msg, sizeof(msg),
			"{\"ok\":true,\"foreground\":%s}", ok ? "true" : "false");
	PipeWriteLine(msg);
}

/*	Where the application lives, and why the plug-in does not go looking.

	The AEGP is in Program Files; the app is wherever it was built or unzipped.
	There is no relationship between those two paths, so any attempt to derive
	one from the other is a guess -- and a guess that is wrong produces a menu
	item that does nothing, which is worse than no menu item.

	So the APP TELLS US. It sends `register_app` with its own executable path on
	every launch, and that is written to AE's preferences under this plug-in's
	key. The consequence is honest and easy to explain: the menu item works after
	the app has been opened once by hand, and says so until then.

	Persistent data rather than a file beside the .aex, because Program Files is
	not writable by a normal user and a plug-in that needs elevation to remember
	something is a plug-in nobody configures. */
static void
LoadAppPath(AEGP_SuiteHandler &suites)
{
	A_Err	err2 = A_Err_NONE, err = A_Err_NONE;

	AEGP_PersistentBlobH blobH = NULL;

	ERR2(suites.PersistentDataSuite4()->AEGP_GetApplicationBlob(
			AEGP_PersistentType_MACHINE_SPECIFIC, &blobH));

	if (!blobH) {
		return;
	}
	ERR2(suites.PersistentDataSuite4()->AEGP_GetString(blobH,
			PHYSBRIDGE_PREF_SECTION, PHYSBRIDGE_PREF_APP, "",
			sizeof(S_app_path), S_app_path, NULL));
}

static void
SaveAppPath(AEGP_SuiteHandler &suites, const char *pathZ)
{
	A_Err	err2 = A_Err_NONE, err = A_Err_NONE;

	AEGP_PersistentBlobH blobH = NULL;

	strncpy_s(S_app_path, sizeof(S_app_path), pathZ, _TRUNCATE);

	ERR2(suites.PersistentDataSuite4()->AEGP_GetApplicationBlob(
			AEGP_PersistentType_MACHINE_SPECIFIC, &blobH));

	if (blobH) {
		ERR2(suites.PersistentDataSuite4()->AEGP_SetString(blobH,
				PHYSBRIDGE_PREF_SECTION, PHYSBRIDGE_PREF_APP, pathZ));
	}
}

//	{"cmd":"register_app","script":"<absolute path to the app exe>"}
static void
DoRegisterApp(AEGP_SuiteHandler &suites, const BridgeRequest *rP)
{
	if (!rP->arg[0]) {
		PipeWriteError("register_app needs the application's path");
		return;
	}
	SaveAppPath(suites, rP->arg);
	Log("  register_app: %s\n", rP->arg);
	PipeWriteLine("{\"ok\":true,\"registered\":true}");
}

/*	Open the application from the Composition menu.

	If it is already running the pipe answers, and starting a SECOND copy would
	put two processes on one work directory, both writing one bake.json. So a
	live bridge client means "it is already open" and the right thing is to say
	so rather than launch again.

	ShellExecute rather than CreateProcess: the app is a normal GUI program, this
	is exactly the "open this document" case, and it does not leave AE holding
	handles to a child it has no interest in. */
/*	The shipped layout: the app sits beside this .aex. That is not a guess --
	both come out of one release folder -- so when it is there it wins over
	whatever was registered, and a copy that moved with the plug-in never needs
	opening by hand. Absent, the registered path still covers dev builds. */
static bool
FindBundledApp(char *outZ, size_t outLen)
{
	HMODULE	self = NULL;
	char	path[MAX_PATH];

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)&FindBundledApp, &self)) {
		return false;
	}
	DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
	if (!n || n >= MAX_PATH) {
		return false;
	}
	char *slash = strrchr(path, '\\');
	if (!slash) {
		return false;
	}
	slash[1] = 0;
	if (strcat_s(path, sizeof(path), PHYSBRIDGE_APP_EXE)) {
		return false;
	}
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
		return false;
	}
	strncpy_s(outZ, outLen, path, _TRUNCATE);
	return true;
}

static void
LaunchApp(AEGP_SuiteHandler &suites)
{
	A_Err	err2 = A_Err_NONE, err = A_Err_NONE;

	char	bundled[MAX_PATH];
	if (FindBundledApp(bundled, sizeof(bundled))) {
		strncpy_s(S_app_path, sizeof(S_app_path), bundled, _TRUNCATE);
	}
	if (!S_app_path[0]) {
		LoadAppPath(suites);
	}
	if (!S_app_path[0]) {
		ERR2(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
				"The physics application is not beside this plug-in ("
				PHYSBRIDGE_APP_EXE "), and has not been opened yet.\r\r"
				"Put it next to PhysBridge.aex, or open it once by hand -- it "
				"registers itself on launch -- and this menu item will work."));
		return;
	}
	if (S_connected) {
		ERR2(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
				"The physics application is already open.\r\r"
				"A second copy would share one work folder with the first and "
				"both would write the same bake."));
		return;
	}

	HINSTANCE rc = ShellExecuteA(NULL, "open", S_app_path, NULL, NULL, SW_SHOWNORMAL);

	if ((INT_PTR)rc <= 32) {
		char msg[1200];

		sprintf_s(msg, sizeof(msg),
				"Could not start the physics application (code %d).\r\r%s\r\r"
				"If it has moved, open it once by hand to re-register it.",
				(int)(INT_PTR)rc, S_app_path);
		ERR2(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg));
	}
}

// ---------------------------------------------------------------------------
//	hooks
// ---------------------------------------------------------------------------

static A_Err
IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long *max_sleepPL)
{
	AEGP_SuiteHandler	suites(sP);
	BridgeRequest		r;

	while (QueuePop(&r)) {
		S_requests++;

		if (!strcmp(r.cmd, "read_scene")) {
			DoReadScene(suites, r.arg);
		} else if (!strcmp(r.cmd, "size_probe")) {
			DoSizeProbe(suites, &r);
		} else if (!strcmp(r.cmd, "send_payload")) {
			DoSendPayload(suites, &r);
		} else if (!strcmp(r.cmd, "pipe_probe")) {
			DoPipeProbe(&r);
		} else if (!strcmp(r.cmd, "bench_keys")) {
			DoBenchKeys(suites, &r);
		} else if (!strcmp(r.cmd, "apply_bake")) {
			DoApplyBake(suites, &r);
		} else if (!strcmp(r.cmd, "focus_ae")) {
			DoFocusAE(suites);
		} else if (!strcmp(r.cmd, "register_app")) {
			DoRegisterApp(suites, &r);
		} else if (!strcmp(r.cmd, "comp_identity")) {
			DoCompIdentity(suites);
		} else if (!strcmp(r.cmd, "_overflow")) {
			PipeWriteError("the request line exceeded the bridge's cap and "
							"was refused");
		} else if (!strcmp(r.cmd, "ping")) {
			PipeWriteLine("{\"ok\":true,\"pong\":true}");
		} else {
			char m[96];
			sprintf_s(m, sizeof(m), "unknown cmd '%s'", r.cmd);
			PipeWriteError(m);
		}
		if (r.dataP) {
			free(r.dataP);		//	ownership ended here -- see BridgeRequest
		}
	}
	return A_Err_NONE;
}

static A_Err
CommandHook(AEGP_GlobalRefcon, AEGP_CommandRefcon, AEGP_Command command,
			AEGP_HookPriority, A_Boolean, A_Boolean *handledPB)
{
	A_Err err = A_Err_NONE;

	if (command == S_status_cmd) {
		AEGP_SuiteHandler	suites(sP);
		char				msg[1024];

		sprintf_s(msg, sizeof(msg),
			"PhysBridge C0 spike\r\r"
			"pipe: %s\r"
			"requests served: %ld\r"
			"last result: %ld bytes in %ld ms\r"
			"last error: %s",
			S_connected ? "client connected" : "listening, no client",
			S_requests, S_last_result_len, S_last_ms,
			S_last_error[0] ? S_last_error : "(none)");

		ERR(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id, msg));
		*handledPB = TRUE;
	} else if (command == S_launch_cmd) {
		AEGP_SuiteHandler suites(sP);

		LaunchApp(suites);
		*handledPB = TRUE;
	}
	return err;
}

static A_Err
UpdateMenuHook(AEGP_GlobalRefcon, AEGP_UpdateMenuRefcon, AEGP_WindowType)
{
	A_Err				err = A_Err_NONE;
	AEGP_SuiteHandler	suites(sP);

	ERR(suites.CommandSuite1()->AEGP_EnableCommand(S_status_cmd));
	ERR(suites.CommandSuite1()->AEGP_EnableCommand(S_launch_cmd));
	return err;
}

static A_Err
DeathHook(AEGP_GlobalRefcon, AEGP_DeathRefcon)
{
	StopPipeServer();

	if (S_cs_ready) {
		DeleteCriticalSection(&S_cs);
		S_cs_ready = FALSE;
	}
	Log("  death: bridge stopped\n");
	return A_Err_NONE;
}

// ---------------------------------------------------------------------------
//	entry point
// ---------------------------------------------------------------------------

A_Err
EntryPointFunc(
	struct SPBasicSuite		*pica_basicP,
	A_long					major_versionL,
	A_long					minor_versionL,
	AEGP_PluginID			aegp_plugin_id,
	AEGP_GlobalRefcon		*global_refconP)
{
	A_Err	err		= A_Err_NONE,
			err2	= A_Err_NONE;

	sP		= pica_basicP;
	S_my_id	= aegp_plugin_id;		//	Commando never does this and registers
									//	every hook under plugin id 0.

	InitializeCriticalSection(&S_cs);
	S_cs_ready = TRUE;

	//	These are the DRIVER version, not the application version -- the first
	//	run logged "AE 126.3" next to an AE that calls itself 26.3x87.
	Log("\nPhysBridge: init (AEGP driver %ld.%ld)\n", (long)major_versionL,
		(long)minor_versionL);

	AEGP_SuiteHandler suites(pica_basicP);

	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_status_cmd));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_status_cmd,
			PHYSBRIDGE_MENU_NAME, AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));

	/*	Composition, not Window: this opens a tool that acts on the comp you
		are in, which is where AE puts Pre-compose and Comp Settings. The
		status item stays under Window, where a diagnostic belongs. */
	ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&S_launch_cmd));
	ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(S_launch_cmd,
			PHYSBRIDGE_LAUNCH_NAME, AEGP_Menu_COMPOSITION,
			AEGP_MENU_INSERT_AT_BOTTOM));

	ERR(suites.RegisterSuite5()->AEGP_RegisterCommandHook(S_my_id,
			AEGP_HP_BeforeAE, AEGP_Command_ALL, CommandHook, 0));
	ERR(suites.RegisterSuite5()->AEGP_RegisterUpdateMenuHook(S_my_id,
			UpdateMenuHook, NULL));
	ERR(suites.RegisterSuite5()->AEGP_RegisterIdleHook(S_my_id, IdleHook, NULL));
	ERR(suites.RegisterSuite5()->AEGP_RegisterDeathHook(S_my_id, DeathHook, NULL));

	if (err) {
		ERR2(suites.UtilitySuite3()->AEGP_ReportInfo(S_my_id,
				"PhysBridge failed to register."));
		return err;
	}
	StartPipeServer();
	return err;
}
