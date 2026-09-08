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

static void
QueuePush(const BridgeRequest *rP)
{
	if (!S_cs_ready) {
		return;
	}
	EnterCriticalSection(&S_cs);

	int next = (S_q_tail + 1) % PHYSBRIDGE_QUEUE_LEN;

	if (next != S_q_head) {		//	drop rather than overwrite a pending one
		S_queue[S_q_tail] = *rP;
		S_q_tail = next;
	}
	LeaveCriticalSection(&S_cs);
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
	Log("  rx: cmd=%s arg=%s\n", r.cmd, r.arg);
	QueuePush(&r);
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
			char	acc[PHYSBRIDGE_LINE_MAX];
			size_t	acc_len = 0;

			for (;;) {
				DWORD got = 0;

				if (!ReadFile(rx, buf, sizeof(buf), &got, NULL) || !got) {
					break;
				}
				for (DWORD i = 0; i < got; i++) {
					char c = buf[i];

					if (c == '\n') {
						acc[acc_len] = 0;
						if (acc_len) {
							HandleLine(acc);
						}
						acc_len = 0;
					} else if (acc_len + 1 < sizeof(acc)) {
						acc[acc_len++] = c;
					} else {
						//	A request longer than the buffer is a protocol
						//	error, not something to silently truncate.
						Log("  rx: request too long, dropped\n");
						acc_len = 0;
					}
				}
			}
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
		} else if (!strcmp(r.cmd, "ping")) {
			PipeWriteLine("{\"ok\":true,\"pong\":true}");
		} else {
			char m[96];
			sprintf_s(m, sizeof(m), "unknown cmd '%s'", r.cmd);
			PipeWriteError(m);
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
			"PhysBridge C0.1 spike\r\r"
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

	Log("\nPhysBridge: init (AE %ld.%ld)\n", (long)major_versionL,
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
