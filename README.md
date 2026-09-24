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

**PhysBridge** — the AEGP. It started as the C0.1 spike and is now the product's
AE half. It listens on a local named pipe and answers:

| command | what it does |
|---|---|
| `ping` | the bridge is up and the plug-in is loaded |
| `read_scene` | runs `b1_read_shapes.jsx` through `AEGP_ExecuteScript` and returns the scene document verbatim |
| `apply_bake` | writes the keyframes **natively**, 131.0 µs/key against ExtendScript's 872.6 |
| `focus_ae` | brings After Effects to the front after an apply |
| `register_app` | remembers where the application is, so the menu item can open it |
| `comp_identity` | the comp's item id and the project's path, so a setup can belong to a comp |
| `size_probe`, `pipe_probe`, `send_payload`, `bench_keys` | the C0.2 and C0.3 harnesses, kept because every native figure is a comparison against them |

And two menu items:

- **Composition → Physics Simulator** opens the application.
- **Window → PhysBridge: bridge status** reports what the pipe has served.

The plug-in keeps the name `PhysBridge` even though the repo is named for the
whole simulator: it is what the built `.aex` is called and what the project
paths reference, and renaming means rebuilding and re-verifying for nothing.

### `b2_apply_bake.jsx` is not deprecated

`apply_bake` does the same job about 7.4× faster, and the ExtendScript stays
anyway. It is the REFERENCE IMPLEMENTATION: every native number is a comparison
against it, and a second implementation you can no longer run is a second
implementation you can no longer check.

### The menu item never guesses where the app is

The AEGP is in Program Files; the app is wherever it was built or unzipped.
There is no relationship between those paths, so anything derived from one to
reach the other is a guess — and a wrong guess makes a menu item that silently
does nothing, which is worse than no menu item.

So the app sends `register_app` with its own executable path on every launch,
and the plug-in keeps it in AE's preferences (not a file beside the `.aex`:
Program Files is not writable by a normal user, and a plug-in that needs
elevation to remember something is a plug-in nobody configures). The consequence
is easy to state — **the menu item works once the app has been opened by hand,
and says exactly that until then.**

A second click does not start a second copy. Two processes on one work directory
both write one `bake.json`, so a live bridge client means "already open".

### What the apply deliberately does not do

C0.3 measured that clearing `SPATIAL_AUTOBEZIER` per key costs more than every
other phase together AND grows per key with the key count — 167 µs/key at 1,000
and 2,765 at 12,000, O(n²) overall. Skipping it leaves the motion path straight,
so it is skipped.

But C0.3 measured **that** and assumed **why**, and the product now depends on
it. So the apply reads the flag back on a spread of keys and reports the count.
If AE ever stops behaving the way C0.3 measured, the window says so instead of
quietly bowing every motion path between keyframes — which is A5's invisible
failure, the one no still frame shows.

Verified on a real comp 2026-09-10: clear on every sampled key.

## How it got here — the C0 spikes

Kept because every native figure above is a comparison against these, and a
measurement whose baseline you can no longer run is a measurement you can no
longer defend.

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

### Identity is a request, not a field in the scene

`comp_identity` exists because the shell keys a comp's pins and per-layer
physics by comp, and the obvious place to put a comp id -- the scene document's
`comp` block, beside name, width and fps -- is the wrong one.

Wall K hashes the **whole raw scene document** (`app/src-tauri/src/verify.rs`).
A comp id inside those bytes would survive that, but a **project path** would
not: Save As between simulate and apply moves the path, moves the hash, and
makes the staleness guard refuse a bake that is still physically valid. A guard
that fires on a correct bake is one people learn to click through.

So identity travels as its own request over the same pipe. It is outside the
hashed bytes by construction rather than by remembering to keep it out,
`b1_read_shapes.jsx` is untouched, and every scene document captured before
this existed still hashes to what it hashed before -- which matters, because
B1's fixture and the September export are evidence rather than just files.

The reply reports rather than infers. An unsaved project has no path and says
`"saved":false` instead of sending something path-shaped; a comp AE would not
identify comes back as `comp_id 0`, and the application has a state for that
which is better than a failed request -- it says the parameters on screen
belong to nothing in particular.

## Protocol

Newline-delimited JSON, one request and one reply.

| request | reply |
|---|---|
| `{"cmd":"ping"}` | `{"ok":true,"pong":true}` |
| `{"cmd":"read_scene","script":"<abs path to .jsx>"}` | the scene JSON, or `{"ok":false,"error":"..."}` |
| `{"cmd":"apply_bake","script":"<abs path to bake.json>"}` | key counts and phase timings, or a refusal naming the mismatch |
| `{"cmd":"focus_ae"}` | `{"ok":true,"foreground":true｜false}` — advisory, and it says which |
| `{"cmd":"register_app","script":"<abs path to the exe>"}` | `{"ok":true,"registered":true}` |
| `{"cmd":"comp_identity"}` | `{"ok":true,"comp_id":N,"comp_name":"...","project_path":"...","project_name":"...","saved":true｜false}` |
| `{"cmd":"size_probe","bytes":"N","mode":"literal"｜"file"}` | a C0.2 report — what was sent, what the script saw |
| `{"cmd":"size_probe","bytes":"N","echo":"1"}` | the N bytes themselves, for the return direction |
| `{"cmd":"send_payload","script":"<abs path>","mode":"literal"｜"file"}` | the same report, for a real file |
| `{"cmd":"pipe_probe","data":"<the payload, inline>"}` | what arrived — length, checksum, head and tail |
| `{"cmd":"bench_keys","bytes":"N"}` | C0.3 timings — µs/key for the add, the interpolation and the tangents |

Every value is a quoted string, including the numbers, so the request reader
stays the one thing that only knows how to pull a quoted value.

The clients are `python-proto/physics_sim/c01_client.py` and `c02_client.py` in
the planning repo.

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

## Running C0.2

No comp is needed and nothing in the project is touched — the probe generates or
reads its own payload and only measures how it travels.

```
python c02_client.py payload b2_bake.json   # the real bake, both roads
python c02_client.py sweep                  # the ceiling, in
python c02_client.py sweep --mode file
python c02_client.py echo                   # the ceiling, out
python c02_client.py pipe                   # the ceiling, over the pipe
python c02_client.py pipe --real            # the real bake, inline
```

`sweep`, `echo` and `payload` measure `AEGP_ExecuteScript` — their payload never
crosses the pipe. `pipe` measures the pipe, and touches no AEGP suite. One
number covering both would describe neither.

`sweep` doubles until AE refuses and then bisects, because the useful form of
"where it breaks" is a byte count, not a doubling step.

## Running C0.3

Nothing of yours is touched: the bridge builds a scratch comp and solid, times
them, and deletes them.

```
python c03_client.py bench              # one run, 6,486 keys
python c03_client.py bench --keys 1000  # a smaller one
python c03_client.py bench --sweep      # several, with pauses between
```

**An earlier version of this exhausted memory badly enough to force a restart**,
and the guard rails come from that. Every call in `AEGP_KeyframeSuite` is
`UNDOABLE`, so 12,000 keys across four phases is ~48,000 operations AE must
retain — and the scratch comp was deleted *inside* the same undo group, so AE
had to keep all of it alive to undo the deletion. Eight runs back to back, with
`AEGP_SetKeyframeFlag` turning out to be O(n²) on top, was too much.

Hence: one run per invocation unless `--sweep`, a 6,486-key cap (B2's real
count — above that was always extrapolation), `mode=full` capped lower still
because it is the quadratic path, and the bridge closes the measured undo group
before it deletes anything.

`--purge` empties AE's undo and image caches afterwards, which is the only thing
that actually *releases* the retained state. It is off by default because it
discards your undo history for the whole project.

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

**The prelude is cleaned up after every run**, and that is not housekeeping.
ExtendScript's global scope lives as long as the AE session, so a leftover
`PHYS_RETURN_JSON` made a *manual* run of `b1_read_shapes.jsx` keep taking the
bridge path — returning the string, saving nothing, showing no dialog, with
nothing to explain it. The bridge now clears its globals (`PHYS_RETURN_JSON`,
`PHYS_P`, `PHYS_F`, `PHYS_EVAL`, `PHYS_ECHO`) in a second, separate
`ExecuteScript` — separate because an appended line does not run when the script
throws, which is when a stale flag does the most damage.

## The application — `app/`

The Tauri shell the plan has been describing since Phase C was decided: a window
that lives *outside* After Effects, talks to this plug-in over the C0.1 pipe, and
owns its own settings.

**Scene | Viewport | Simulation.** Read the comp, pin layers, set physics per
layer, simulate, **scrub the result**, apply. The viewport takes the middle
because it is the only panel whose usefulness scales with width.

Two things about it are worth knowing before reading the code:

- **The scrubber is continuous** — twentieths of a frame, quarter-frame steps.
  A5's finding is that a wrapped rotation is invisible on every still and the
  damage lives inside one frame interval; a per-frame scrubber can only show the
  values we already know are correct. In the C1.2 sitting a layer crossed 180°
  between frames 69 and 70 and the sampler stepped straight over it.
- **The front end owns the transform and nothing else.** Geometry arrives
  already flattened as `ae-physics-render/1`, because turning a comp into
  polygons is bezier flattening, group transforms, layer scale and convex
  decomposition — verified Python that must not be reimplemented in TypeScript.
  `c2_render_model.py` checks the app's arithmetic against `preview.py` at
  fractional frames.

Rust parses neither the scene nor the bake. Those already have two
implementations that must agree, and a third would be a third place to drift.

It lives in this repo because one clone then gets both halves of the product.
The cost is a node/cargo project inside the SDK `Examples` tree, which is a wart
and not a problem — nothing in the MSBuild solution knows `app/` exists.

```powershell
cd app
npm install
npm run tauri dev      # or: npm run tauri build
```

What it does, and the whole of what it does:

| | |
|---|---|
| **bridge light** | pings the AEGP, so "is this thing connected" is answered before anything else fails |
| **read comp** | `read_scene` over the pipe, running `b1_read_shapes.jsx` inside AE. The document is written to the app's work folder, because B3 takes a **file** and hashes it — that is Wall K's mechanism, and C3 depends on it surviving the GUI |
| **scene list** | comp facts, layers, and the reader's own warnings. Clicking a layer pins it: B3's `--static`, which is the one parameter that is per-layer |
| **parameters** | B3's set, as controls, at B3's defaults. Nothing here is a second implementation of the loop — every control is an argument to `b3_loop.py` |
| **simulate** | runs that command and reports what it said, including the escape refusal (exit 3) as its own outcome rather than as a failure |
| **settings** | interpreter and prototype folder, saved on every change |

### What it deliberately is not

**It does not know the schema.** The scene document is parsed in the front end,
shallowly — comp facts, names, ids, warnings. `ae-physics-scene/2` already has
two implementations that have to agree, `scene_io.py` and the jsx, and the plan
already worries about those drifting; a third in Rust, reading the same fields
to build a list, is a third place to get it wrong for nothing.

**It does not apply the bake.** That is still `b2_apply_bake.jsx` by hand. And
it does not show you the bake before you apply it — that is C2, and A5 is the
argument for why looking matters.

### The re-arm race, which is worth knowing about

The pipe server holds **one instance** of each pipe and re-creates them between
clients. A `CreateFile` landing inside that window connects to an instance the
server is about to tear down: the request is read and answered, and the answer
is written to nobody —

```
pipe: client connected
rx: cmd=ping ...
pipe: client gone
write: no client connected, 23 bytes dropped
```

23 bytes is exactly `{"ok":true,"pong":true}`. `c01_client.py` never hit this
because a person runs it once; an application pings on launch and then reads,
back to back. `app/src-tauri/src/bridge.rs` retries the whole exchange when the
connection closes without an answer, and *only* then — a connection that drops
mid-reply is an answer, and re-running it would re-run whatever the bridge
already did.

## Status

**Everything through C2 is done and verified in real After Effects (26.3x87).**

| | |
|---|---|
| C0.1 the bridge | byte-identical scene document, 16 ms round trip |
| C0.2 payload size | no ceiling below 32 MB, 231× the bake |
| C0.3 native keyframes | 88.5 µs/key for the interpolation pass vs 853 |
| C1 the loop | AE holds the solver to **0.000057 px/deg**, straight tweens |
| the apply | **131.0 µs/key, 28.6×** the ExtendScript path |
| C2 the viewport | 15/15 offline, agreeing with `preview.py` at fractional frames |
| C3 Wall K | refuses a stale bake live, and the override applies |
| C6.3 rest preview | the comp draws before it is simulated; a bake from another scene is not drawn over it |
| C7.1 per-comp setup | pins and overrides belong to the comp they were set on |

`SPIKES.md` has the numbers and, more usefully, what each result does **not**
cover. Two of those limits are worth repeating here:

- **A per-key figure is only meaningful from a Release build.** The first
  measurement of the native apply read 262.4 µs/key against a projection of
  117.7 — 2.2× the wrong way, on work that should have been cheaper. The `.aex`
  had been built `Configuration=Debug`, which is `/Od`. What caught it was the
  direction: a number merely worse than hoped invites a story, a number worse in
  an impossible direction means the setup is wrong.
- **Nothing offline can check that the canvas draws what the numbers say.** A
  correct polygon list and a wrong fill rule look identical to `c2_render_model.py`.

### What is open

- **C7** (beyond C7.1) — saving a sim with the project. Waiting on a decision rather than on
  work: is a sim part of the artwork or part of the working state? See the
  roadmap.
