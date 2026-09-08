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
	}
	return err;
}

static A_Err
UpdateMenuHook(AEGP_GlobalRefcon, AEGP_UpdateMenuRefcon, AEGP_WindowType)
{
	A_Err				err = A_Err_NONE;
	AEGP_SuiteHandler	suites(sP);

	ERR(suites.CommandSuite1()->AEGP_EnableCommand(S_status_cmd));
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
