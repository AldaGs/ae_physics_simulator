# ae_physics_simulator — the native side

The C++ half of a Newton-style 2D physics simulator for After Effects: read comp
layers, simulate them as rigid bodies, bake position and rotation back onto the
layers as keyframes.

**The plan, the roadmap and the entire verified pipeline live in
[`AldaGs/ae-py-planning`](https://github.com/AldaGs/ae-py-planning).** Start
there — `ae-physics-sim-roadmap.md` for what to do next,
`ae-physics-sim-plan.md` for why, and `python-proto/physics_sim/WALKTHROUGH.md`
for every measurement behind both. This repo holds only what has to be native.

## Why anything here is native at all

Two reasons, and they are the whole justification for a C++ component:

1. **Alpha is invisible to script.** Neither ExtendScript nor CEP can read
   rendered pixels, so collision shapes derived from a layer's actual silhouette
   require an AEGP calling `AEGP_RenderAndCheckoutFrame`. Newton interprets
   precomp and footage layers *as rectangles*; this is where that stops being
   true. (Wall A.)
2. **The application lives outside AE.** The UI is a separate process, so
   something inside AE has to answer it. That something is this plug-in.

Since the AEGP must exist for (1) regardless, it is also the bridge for (2) —
one bridge instead of building a CEP panel and throwing it away.

## What is here now

**PhysBridge** — the C0.1 spike, and the seed of the AEGP. It listens on a local
named pipe, runs the project's existing ExtendScript through
`AEGP_ExecuteScript`, and returns the result down the pipe.

The plug-in keeps the name `PhysBridge` even though the repo is named for the
whole simulator: it is what the built `.aex` is called and what the project
paths reference, and renaming means rebuilding and re-verifying for nothing.

### The question C0.1 asks

Can the AEGP hand back a scene document **byte-identical** to what the reader
script's own save dialog writes? If yes, Phase C's architecture stands and
`b1_read_shapes.jsx` / `b2_apply_bake.jsx` — already verified against real AE —
are *reused* rather than rebuilt as `evalScript` calls. If no, Phase C changes
shape.

### Design, and what it borrows

Transport and threading come from [`AldaGs/pieFX`](https://github.com/AldaGs/pieFX),
where both are already proven inside AE:

- **Two half-duplex pipes, not one duplex pipe.** The UI thread writes to TX and
  the background thread parks on RX, so a write can never queue behind a
  blocking read.
- **AEGP calls only ever on AE's UI thread.** A request arriving on the pipe
  thread is queued; the idle hook drains it. No AEGP suite may be touched off
  the UI thread.
- **The entry point is modelled on `Persisto`, never `Commando`.** The real
  `AEGP_PluginInitFuncPrototype` takes five parameters, and a different
  signature is a legal C++ overload: it compiles, links, exports mangled, and
  leaves AE saying *"Couldn't find main entry point (48 :: 72)"*.

Two things it deliberately does **not** borrow:

- pieFX keeps a script's return value in a fixed 2 KB buffer — right for its
  toasts, fatal here, where the point is a document of unbounded size. The
  result is streamed straight from the `AEGP_MemHandle` to the pipe and its
  length logged, which answers most of **C0.2** on the *return* direction for
  free.
- `PipeWrite` loops rather than trusting a single `WriteFile`. A short write
  treated as a whole one is exactly how a payload-size spike gets a false pass.

## Where it must live to build

The project's include paths are relative to the SDK's `Examples` directory, so
this repo is cloned **into the SDK tree**:

```
C:\AE_SDK\ae25.6_61.64bit.AfterEffectsSDK\Examples\Template\PhysBridge\
```

Depth matters: pieFX sits five levels below `Examples` and uses `..\..\..\..\..\`;
this sits three and uses `..\..\..\`.

## Build

**After Effects locks a loaded `.aex` — close AE before rebuilding.**

```powershell
$env:AE_PLUGIN_BUILD_DIR = "C:\AE_SDK\_build_out\"
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\Win\PhysBridge.sln" -p:Configuration=Debug -p:Platform=x64
```

Then the ten-second check that catches the entire "couldn't find main entry
point" class — the export must be a **bare** `EntryPointFunc`:

```powershell
dumpbin /EXPORTS C:\AE_SDK\_build_out\AEGP\PhysBridge.aex | findstr EntryPointFunc
```

## Deploy (needs admin)

AEGPs go in a folder under AE's own `Plug-ins`, **not** the shared MediaCore
path the effect plug-ins use — MediaCore is shared with Premiere, which has no
AEGP host.

```powershell
Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile','-Command',
  'New-Item -ItemType Directory -Force "C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\PhysBridge" | Out-Null;
   Copy-Item -Force "C:\AE_SDK\_build_out\AEGP\PhysBridge.aex" "C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\PhysBridge\"'
```

Confirm it loaded with **Window → PhysBridge: bridge status**: it reports whether
a client is connected, how many requests it has served, and the size and
duration of the last result. A log is also appended to `%TEMP%\physbridge.log`.

## Protocol

Newline-delimited JSON, one request and one reply.

| request | reply |
|---|---|
| `{"cmd":"ping"}` | `{"ok":true,"pong":true}` |
| `{"cmd":"read_scene","script":"<abs path to .jsx>"}` | the scene JSON, or `{"ok":false,"error":"..."}` |

The client is `python-proto/physics_sim/c01_client.py` in the planning repo.

## Running the spike

The pass criterion is "byte-identical to what the save dialog writes", and that
only means anything if both readings see the **same comp** — so both happen in
one sitting:

1. Open the comp. **File → Scripts → Run Script File…** → `b1_read_shapes.jsx`,
   save as `c01_dialog.json`.
2. ```
   python c01_client.py ping
   python c01_client.py read_scene --out c01_bridge.json
   python c01_client.py verify c01_dialog.json c01_bridge.json
   ```

## The one change this forced on the ExtendScript

`b1_read_shapes.jsx` ended in a save dialog and so had nothing to return. It now
builds the JSON **once** and only the destination differs: the dialog path is
unchanged, and when the caller prepends `var PHYS_RETURN_JSON = true;` it
returns the string instead. The bridge prepends exactly that one line and runs
the rest verbatim — which is what makes the byte comparison meaningful, since
both paths format with the same code.

It also had to stop raising modals in that mode. An `alert()` from a script the
AEGP started **inside its idle hook** would block AE waiting for a click nobody
is there to give, so failures come back as a string the bridge turns into a JSON
error.

## Status

Builds clean, exports a bare `EntryPointFunc`, client written and failing
cleanly when AE is absent. **Not yet run in After Effects.**
