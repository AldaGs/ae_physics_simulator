/*
	PhysBridge - C0.1 spike for the AE physics simulator.

	THE QUESTION
	------------
	Can an AEGP take a request over a local pipe, run the physics sim's existing
	ExtendScript reader through AEGP_ExecuteScript, and hand the scene JSON back
	down the pipe -- byte-identical to what the script's own save dialog writes?

	If yes, Phase C's architecture stands: a Tauri shell outside AE talking to a
	native bridge inside it, with B1/B2's already-verified .jsx files reused
	rather than rebuilt as evalScript calls. If no, Phase C changes shape.

	THROWAWAY. One command, no settings, no persistence, Windows only. It exists
	to answer a question, and the answer belongs in the plan, not in this code.

	WHAT IT BORROWS FROM pieFX  (Examples/Template/pieFX/poc/native)
	---------------------------------------------------------------
	The transport and the threading model, both already proven in AE:

	  - two half-duplex named pipes rather than one duplex pipe. The UI thread
	    writes to TX and the background thread parks on RX, so a write can never
	    queue behind a blocking read.
	  - AEGP calls happen ONLY on AE's UI thread, inside the idle hook. A
	    request arriving on the pipe thread is pushed onto a queue; the idle hook
	    drains it. No AEGP suite may be touched off the UI thread.
	  - the entry point is modelled on Persisto, never Commando: the real
	    AEGP_PluginInitFuncPrototype takes FIVE parameters, and a different
	    signature is a legal C++ overload that compiles, links, exports mangled,
	    and leaves AE saying "Couldn't find main entry point".

	WHAT IT DELIBERATELY DOES NOT BORROW
	------------------------------------
	pieFX keeps a script's return value in a fixed 2 KB buffer, which is right
	for its toasts and fatal here: the whole point is a scene document of
	unbounded size. The result is streamed straight from the AEGP_MemHandle to
	the pipe, and its length is logged -- which is most of C0.2's answer for
	free.
*/

#pragma once

#include "AEConfig.h"

#ifdef AE_OS_WIN
	#define VC_EXTRALEAN
	#include <windows.h>
#endif

#include "entry.h"
#include "AE_GeneralPlug.h"
#include "AEGP_SuiteHandler.h"
#include "AE_Macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define PHYSBRIDGE_NAME			"PhysBridge C0 spike"
#define PHYSBRIDGE_MENU_NAME	"PhysBridge: bridge status"

//	Distinct from pieFX's names so both plug-ins can be loaded at once. A spike
//	that forces you to uninstall the thing you are modelling it on is a spike
//	you will run less often.
#define PHYSBRIDGE_PIPE_TX		"\\\\.\\pipe\\aephys_bridge_tx"
#define PHYSBRIDGE_PIPE_RX		"\\\\.\\pipe\\aephys_bridge_rx"

#define PHYSBRIDGE_LINE_MAX		(1 << 16)	// request line; requests are tiny
#define PHYSBRIDGE_QUEUE_LEN	8

//	A request, parsed on the pipe thread and executed on the UI thread.
typedef struct {
	char	cmd[32];
	char	arg[MAX_PATH];		// for read_scene: the .jsx path
} BridgeRequest;

extern "C" {
	DllExport A_Err EntryPointFunc(
		struct SPBasicSuite		*pica_basicP,
		A_long					major_versionL,
		A_long					minor_versionL,
		AEGP_PluginID			aegp_plugin_id,
		AEGP_GlobalRefcon		*global_refconP);
}
