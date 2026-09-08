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

## C0.2 — payload size — **PASS** (2026-09-08, AE 26.3x87)

**The answer: there is no ceiling anywhere near the bake.** Both roads carry the
real 145,090-byte `b2_bake.json` intact, and neither has a limit below **32 MB**
— 231× the bake — in either direction. The sweep never got to bisect.

```
b2_bake.json -- 145,090 bytes, both ingestion paths

  literal   145,090 B  script  146,048 B   93 ms  sum  64 ms  eval  20 ms  4 keys  intact
  file      145,090 B  script    1,030 B   93 ms  sum  65 ms  eval  24 ms  4 keys  intact
```

Five repeats: literal 93–109 ms, file 94–109 ms. The two roads are **the same
speed**, and the difference between them is smaller than the instrument can
see — the bridge times with `GetTickCount`, whose resolution is ~15.6 ms, which
is why every number in the sweeps is a multiple of about 15.

The sweeps, doubling from 16 KB to 32 MB, all intact at every step:

```
literal   33,554,432 B  script 33,555,309 B  16094 ms  sum 15604 ms  intact
file      33,554,432 B  script      1,031 B  15781 ms  sum 15688 ms  intact
echo      33,554,432 B  back   33,554,432 B  16847 ms                intact
```

### What the timings actually say, which is not what they look like

Almost all of that wall time is **the probe's own checksum**, not transport. At
33 MB the literal road took 16,094 ms and the ExtendScript checksum loop
accounted for 15,604 ms of it — so moving 32 MB into AE cost something like
490 ms, and the measurement is mostly measuring the ruler.

That is the real finding underneath the ceiling. Cost here scales with
**ExtendScript touching characters**, not with bytes crossing a boundary. The
checksum ran at roughly 0.45 µs per character; transport barely registers.

For the bake specifically: of ~94 ms, about 66 ms is the checksum (the probe's,
which B2 will never run) and ~22 ms is `eval` (which B2 already pays today,
either way). What remains for the transfer itself is under one tick of the
clock — **below the resolution of the instrument**, in both roads.

### So which road?

The measurement does **not** pick a winner: they cost the same. It only
establishes that the literal road is *viable*, which was the open question. The
choice therefore falls to grounds other than speed:

- **file** — B2 already works this way, so it needs only the one-line prelude B1
  got. The request stays tiny. Nothing is escaped into code.
- **literal** — nothing touches the disk: no temp file to write, collide on, or
  clean up, which is worth something to a shell that may run more than one sim.

**Leaning file**, on the grounds that it is less code and less risk: escaping a
payload into script text is an injection surface and up to a 2× size expansion,
and it buys no time back. But this is now a design preference with a
measurement behind it, not a gamble — which is the whole point of the spike.

### The gap this opens, and it is the next thing to settle

The literal road is **not usable end to end today**, for a reason outside what
was measured. The payload here was generated or read *inside* the bridge, so
this ceiling is `AEGP_ExecuteScript`'s alone. Getting the bake from the shell to
the bridge is a different limit: `PHYSBRIDGE_LINE_MAX` is 64 KB, and
`HandleLine` **drops** a longer request rather than truncating it. A 145 KB bake
sent inline over the pipe would not arrive at all.

That is one constant to raise — or moot, if the shell passes a path, which is
the file road. It is called out here rather than quietly fixed because it is a
measurement nobody has taken: **the pipe has not been tested above 16 KB in the
request direction.**

### What made the result trustworthy

Every run checks integrity, not just survival, because a truncated payload that
still parses is C0.1's false pass wearing a different hat. The probe script
checksums what it received, the bridge checksums what it sent, and the client
compares them; head and tail character codes would have located any cut. The
checksum is written three times — C++, ExtendScript, Python — and all three were
made to agree **offline under `node`** against a real JSON payload before AE was
involved. `eval` is included because a payload that arrives intact but will not
parse is still a failure; all runs reported `4 keys` and no parse error.

One thing deliberately not treated as corruption: `chars` ≠ `bytes_sent`.
ExtendScript counts UTF-16 code units and the bridge counts bytes, so they agree
only for ASCII — the bridge reports which it was, and the client decides.

---

## C0.2 — how it was built

### The question, in the form it actually takes

The roadmap asks: does `AEGP_ExecuteScript` accept a 148 KB bake as a string
argument, or must the script read a temp file?

Reading `b2_apply_bake.jsx` sharpens that. B2 **already** reads its bake from a
file — `File.openDialog` → `read()` → `eval` — so the temp-file road is not a
hypothesis to be tested; it is the incumbent, and it works today by hand. The
open question is whether the *other* road is viable: escaping the bake into the
script text and handing the whole thing over in one call.

That matters because the two roads lead to different products:

- **literal** — the shell hands the bake to the AEGP and nothing touches the
  disk. No temp file to write, collide on, or clean up.
- **file** — B2 gets the same one-line prelude B1 got (`var PHYS_BAKE_PATH` in
  place of the dialog) and the bake goes via `%TEMP%`.

Either answer is cheap to act on. Not knowing is what costs.

### What the harness measures

Three commands on the bridge, driven by `python-proto/physics_sim/c02_client.py`:

| | |
|---|---|
| `size_probe` | N synthetic bytes in, a small report back — the sweep |
| `size_probe` + `echo` | N bytes in, the same N bytes back — the *return* direction |
| `send_payload` | the real `b2_bake.json` (145,090 bytes), both roads |

The sweep doubles until AE refuses and then **bisects**, because "somewhere
between 8 MB and 16 MB" is not a limit anyone can design to. The number wanted
is a byte count.

The integrity checking, and the reason the pipe was kept out of scope, are
described under the result above — they are what make the number mean
something, so they belong next to it.

### Status

Run inside After Effects on 2026-09-08. `read_scene` was untouched, so C0.1
stays runnable, and the corrected init label is now live in the log
(`AEGP driver 126.3`).

## C0.3 — keyframes from native code — not started

Can `AEGP_KeyframeSuite` beat ExtendScript's measured 853 µs/key interpolation
cost? Wall I's only remaining lever, and fracture makes it a requirement rather
than an optimisation: fifty shards is ~25 s of interpolation alone.
