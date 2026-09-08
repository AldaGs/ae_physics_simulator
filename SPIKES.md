# Spike log

Measurements, and what each one actually proves. The planning repo's
`WALKTHROUGH.md` is the model: a result is a number with a stated scope, not a
verdict.

---

## C0.1 — the bridge — **PASS** (2026-09-07, AE 26.3x87)

**The question.** Can an AEGP take a request over a local pipe, run
`b1_read_shapes.jsx` through `AEGP_ExecuteScript`, and hand the scene JSON back
down the pipe byte-identically to what the script's own save dialog writes?

**The answer.**

```
dialog :    2185 bytes  sha256 b589df0b0325b543  c01_dialog.json
bridge :    2185 bytes  sha256 b589df0b0325b543  c01_bridge.json

PASS  the bridge returns exactly what the save dialog writes
```

From the bridge's own log:

```
pipe: client connected
rx: cmd=read_scene arg=...\b1_read_shapes.jsx
read_scene: ...\b1_read_shapes.jsx (15927 bytes of script)
read_scene: 2185 bytes returned in 16 ms
```

Comp `sim 2`, 1000x500, three shape layers, no warnings. **16 ms** for the whole
round trip inside AE: read the file, run the script, return the string.

### What this settles

Phase C's architecture stands. A Tauri shell outside AE can drive a native
bridge inside it, and — the part that actually saves work — `b1_read_shapes.jsx`
and `b2_apply_bake.jsx` are **reused rather than rebuilt** as `evalScript`
calls. The earlier plan's "rebuild B1/B2 for the panel" is deleted work.

### What it does NOT settle

**C0.2 is still open, and this run makes only a dent in it.** The payload here
was 15,927 bytes *in* (the script) and 2,185 bytes *out* (the scene). The
question that matters is a **148 KB bake** going the other way, and 2 KB out is
no evidence about 148 KB in. What this does establish is that neither direction
has a small hard ceiling, and that the plumbing is honest about size: nothing in
the result path uses a fixed buffer, and `PipeWrite` loops rather than trusting
one `WriteFile`.

### Everything it got right first time, and why

Almost nothing here was invented. The two decisions that would have cost a
debugging session each were taken from `pieFX`, where they had already been paid
for:

- **AEGP calls only on AE's UI thread**, via the idle hook, with pipe requests
  queued from the background thread. Calling a suite off the UI thread is the
  kind of failure that looks like random corruption.
- **The entry point modelled on `Persisto`, not `Commando`.** `dumpbin` showed a
  bare `EntryPointFunc` on the first build. Commando's stale seven-parameter
  signature is a legal C++ overload that compiles, links, exports mangled, and
  leaves AE saying "Couldn't find main entry point (48 :: 72)".

The one thing that did need fresh thought was the script side, below.

### The change it forced, and the modal that would have hung AE

`b1_read_shapes.jsx` ended in a save dialog, so it had nothing to return. It now
builds the JSON **once** and only the destination differs; the bridge prepends a
single `var PHYS_RETURN_JSON = true;` line and runs the rest verbatim. That is
what makes the byte comparison mean anything — both paths format with the same
code, so a difference could only have come from the bridge.

The subtler half: the script also had to stop raising modals in that mode. An
`alert()` from a script the AEGP started **inside its idle hook** would block AE
waiting for a click nobody is there to give — a hang, not an error. Every
user-facing message is now guarded, and failures come back as a string the
bridge converts to a JSON error.

### Why the comparison used a fresh comp

The pass criterion is only meaningful if both readings see the **same** comp, so
both were taken in one sitting. Comparing against the stored `b1_ae_export.json`
would have failed for a reason that has nothing to do with the bridge: B2 wrote
keyframes onto those layers, so the reader now emits an extra warning about
pre-existing Position keys. The comp used here (`sim 2`) is clean, which is why
`warnings` is empty.

### Loose end

The init line logs `AE 126.3`, which is not the application version — it is
`driver_major_versionL` / `driver_minor_versionL` from the entry point. The
label has been corrected in source; **the binary that produced the result above
predates that change**, which is cosmetic and touches nothing on the tested path.

---

## C0.2 — payload size — not started

Does `AEGP_ExecuteScript` accept a 148 KB bake as a string argument, or must the
script read a temp file? Measure **where it breaks**, not just whether it works.

## C0.3 — keyframes from native code — not started

Can `AEGP_KeyframeSuite` beat ExtendScript's measured 853 µs/key interpolation
cost? Wall I's only remaining lever, and fracture makes it a requirement rather
than an optimisation: fifty shards is ~25 s of interpolation alone.
