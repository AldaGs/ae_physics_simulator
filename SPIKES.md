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

### Re-verified 2026-09-08, and the defect that re-verifying found

Re-run against the current binary, after the RX accumulator was rewritten and
the globals fix went in — same comp, same sitting, byte-identical, and the same
sha256 as the original:

```
dialog :    2185 bytes  sha256 b589df0b0325b543  c01_dialog_recheck.json
bridge :    2185 bytes  sha256 b589df0b0325b543  c01_bridge_recheck2.json

PASS  the bridge returns exactly what the save dialog writes
```

**The first attempt at this could not be done at all, and that was the finding.**
The reader chooses its output path from a global:

```javascript
var BRIDGE = (typeof PHYS_RETURN_JSON !== "undefined") && PHYS_RETURN_JSON;
```

The bridge prepends `var PHYS_RETURN_JSON = true;`, and **ExtendScript's global
scope lives as long as the AE session**, shared with every script the user runs
by hand. So after any bridge read, a *manual* run of `b1_read_shapes.jsx` kept
taking the bridge path: it returned the string, saved nothing, and showed no
dialog — with no error to explain why.

Two things follow, and the second is the one that matters:

- **C0.1's procedure had an undocumented order dependency.** The original pass
  ran the dialog first and the bridge second. Reversed, there is no dialog and
  no file. "Both readings in one sitting" was never enough; the sitting was
  ordered, and nothing said so.
- **The bridge was mutating shared state inside AE.** In Phase C, anyone who ran
  B1 by hand after using the app would have got no dialog and no saved file, and
  nothing to diagnose it with. A defect in the product, not the spike.

The same leak applied to C0.2's probes, where `PHYS_P` holds the payload — a
32 MB probe left 32 MB alive in AE for the rest of the session.

**Fixed:** every script the bridge runs is now followed by a second, tiny
`ExecuteScript` that clears `PHYS_RETURN_JSON`, `PHYS_P`, `PHYS_F`, `PHYS_EVAL`
and `PHYS_ECHO`. Its own call rather than an appended line, because an appended
line does not run when the script throws — which is exactly when a stale flag
does the most damage. It is called before every early return, and timed outside
the C0.2 stopwatch so those numbers stay measurements of the transfer.

The re-run above was deliberately done **bridge first, dialog second** — the
order that used to fail. The dialog appeared. That is what makes this a
demonstration of the fix rather than an assumption about it, and it means the
same-sitting comparison no longer depends on order.

C0.2 was re-measured on the fixed build too: 94–109 ms on both roads, the bake
inline over the pipe in 13 ms, and the literal sweep still reaching 32 MB
intact.

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
payload into script text that is then executed is a second place to get it
wrong, and it buys no time back. (I also gave size expansion as a reason; the
pipe measurement below withdraws that one — it costs 0.06%, not 2×.) This is now
a design preference with a measurement behind it, not a gamble, which is the
whole point of the spike.

### The pipe — the other half, measured (2026-09-08)

The numbers above are `AEGP_ExecuteScript`'s alone: the payload was generated or
read *inside* the bridge and never crossed the pipe. Getting the bake from the
shell *into* the bridge was a separate limit, untested above 16 KB, and
`PHYSBRIDGE_LINE_MAX` was a 64 KB constant that `HandleLine` enforced by
**dropping** longer requests. That constant was arbitrary — it was written when
every request was a path and a number — and leaving it to be read as a finding
would have been the worst outcome, so the accumulator now grows on demand and
the pipe was measured properly.

`pipe_probe` puts the payload in the request line itself, which is what a shell
handing over a bake actually does, and touches no AEGP suite — so what it
measures is the transport and the request reader, nothing else.

```
       16,384 B payload       16,418 B line    43 ms   intact
      262,144 B payload      262,178 B line    19 ms   intact
    4,194,304 B payload    4,194,338 B line    76 ms   intact
   33,554,432 B payload   33,554,466 B line   426 ms   intact

   no ceiling in the request direction below 33,554,432 bytes
```

**The pipe is not a constraint, and it is not even slow.** 32 MB inline in
426 ms is about 79 MB/s — for scale, the ExtendScript checksum over that same
32 MB took *sixteen seconds*. Below ~1 MB the numbers are a latency floor of
20–45 ms (the idle hook's polling), not throughput.

The real bake, inline, over three runs:

```
   145,090 B payload   145,206 B line   26 / 15 / 14 ms   intact
```

**This corrects something I argued above.** I gave "up to a 2× size expansion"
as a reason to prefer the file road. Measured, the escaping costs **82 bytes on
145,090 — 0.06%**, because `b2_bake.json` is one line of numeric JSON with 82
quotes, no backslashes and no newlines. The 2× is a worst case that this payload
comes nowhere near, so that argument is withdrawn. What survives is the
structural one: the literal road escapes a payload into source that is then
executed, so it has two places to get wrong where the file road has one.

The over-cap path was checked too, since it is new behaviour and the old code
failed silently:

```
   67,108,864  REFUSED: the request line exceeded the bridge's cap and was refused
```

Refused with a message rather than dropped without one — a request that vanishes
in silence gets diagnosed as a hung bridge. The 64 MB cap is a guard against a
client that never sends a newline, not a measurement.

### Re-run after the change

The RX accumulator sits underneath every request, so the results above were
re-measured on the new build rather than inherited: `payload` gave 94 ms on both
roads (unchanged), and all three sweeps still reach 32 MB intact. C0.1 was
re-verified separately and passes byte-identically — see its section above, and
the globals leak that re-verifying it exposed.

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

## C0.3 — keyframes from native code — **PASS** (2026-09-08, AE 26.3x87)

> The first version of this harness exhausted memory on the test machine badly
> enough to require a restart. The fixes are in and the result below was
> re-measured on the fixed harness — see "What went wrong" for what caused it
> and what the guard rails now are.

Can `AEGP_KeyframeSuite` beat ExtendScript's measured 853 µs/key interpolation
cost? Wall I's only remaining lever, and fracture makes it a requirement rather
than an optimisation: fifty shards is ~25 s of interpolation alone.

### Reading the suite sharpened the question before any code ran

B2's measurement, from `WALKTHROUGH.md`:

| | µs per key | 12,000 keys |
|---|---|---|
| `setValueAtTime` in a loop | 6,850 | 82 s |
| `setValuesAtTimes` in bulk | 19.6 | 0.2 s |
| forcing LINEAR interpolation | 853 | 10 s |

The value writes are already free; the interpolation pass is Wall I. And
`AEGP_KeyframeSuite5` turns out to have **the same shape**: there is a batch add
(`StartAddKeyframes` / `AddKeyframes` / `SetAddKeyframe` / `EndAddKeyframes`,
the native `setValuesAtTimes`), but `AEGP_SetKeyframeInterpolation` is
**per-key with no bulk form either**.

So going native does not change the *shape* of the work — it changes who pays
for each call, which makes the real question:

> **Is 853 µs/key the ExtendScript bridge, or is it AE doing the work?**

Only the first goes away. C0.2 found ExtendScript's per-character cost dominated
everything around it, which is grounds for suspecting the bridge — but a
per-character tax is not a per-call one, and suspicion is not a measurement.

- near **20 µs/key** → Wall I is solved and fracture becomes affordable.
- near **853** → the cost is AE's, native buys nothing, and the answer is
  decimation or AE's default-interpolation preference, which B2 flagged as
  untested and still is.

### How it measures

`bench_keys` times three phases separately, because lumping them would hide
which one is the problem — and B2's whole finding was that the phases differ by
two orders of magnitude:

| phase | the ExtendScript it is compared against |
|---|---|
| batch add | `setValuesAtTimes`, 19.6 µs/key |
| interpolation | `setInterpolationTypeAtKey`, 853 µs/key |
| spatial tangents | the other half of B2's pass, zeroed so the path cannot bow |

Tangents are included deliberately. Leaving them out would have made the native
path look better than it is, since B2 pays for them too.

**It builds its own scratch comp and solid, times those, and deletes them**, all
inside one undo group. Writing tens of thousands of keyframes into whatever the
user has open would be rude, and it would make the numbers depend on their
project rather than on AE.

**Timed with `QueryPerformanceCounter`**, not the `GetTickCount` the rest of the
bridge uses. That clock's ~15.6 ms resolution is why every C0.2 number is a
multiple of about fifteen; at 853 µs/key a thousand keys is under one tick, so
it would have measured nothing here.

It reads a keyframe back before reporting — fast and wrong is not a result — and
reports the stream's dimensionality rather than assuming it, since B2 was bitten
by a 2D Position whose *spatial tangents* demanded three elements.

### What went wrong, and it was the harness

The full sweep — eight benchmarks back to back, four key counts × two modes —
exhausted memory on the test machine and forced a restart. Three causes, all
mine, and the first is visible in the SDK header if you read it for anything
other than signatures:

- **Every call in `AEGP_KeyframeSuite` is marked `/* UNDOABLE */`.** At 12,000
  keys × four phases that is ~48,000 undoable operations for AE to retain, and
  because the scratch comp is deleted *inside* the same undo group, AE must keep
  all of it alive to be able to undo the deletion.
- **`AEGP_SetKeyframeFlag` is O(n²)** (see below). 12,000 calls each apparently
  re-walking a 12,000-key array is enormous allocation churn; that phase alone
  took 90 s on the last run, up from 33 s on the one before, which was itself
  the machine already struggling.
- **Nothing was purged between runs**, so eight of these accumulated.

**All four fixes are now in:**

| | |
|---|---|
| the measured undo group closes *before* anything is deleted | teardown gets its own small group, so the two are never one retained entry |
| capped at 6,486 keys | B2's real count; 20,000 hard limit in the bridge |
| `mode=full` capped lower still, at 3,000 | it is the quadratic path and its answer is already known |
| one run per invocation | `--sweep` must be asked for, and pauses between runs |

Plus `--purge`, which runs `app.purge(PurgeTarget.UNDO_CACHES)` through the
bridge's own `AEGP_ExecuteScript`. That is the only thing that actually
*releases* the retained state rather than bounding it — there is no purge in the
AEGP suites, only `AEGP_Menu_PURGE` for inserting a menu item. It is **opt-in
and stays that way**: it discards the user's undo history for their whole
project, and wiping somebody's undo stack to tidy up after a benchmark is not a
trade this code gets to make on their behalf.

Ending the group earlier bounds the *shape* of what AE retains; it does not
empty it. Only the purge does. Both are worth having and neither is a
substitute for the other.

### The result (2026-09-08, AE 26.3x87, on the fixed harness)

**Wall I is solved, and it was the ExtendScript bridge all along.** The LINEAR
pass costs **88.5 µs/key** natively against ExtendScript's 853 — about **10×** —
and it is flat:

```
mode=nobezier
   1,000 keys   add 30.1   interp 26.8   tangents 59.0   =  85.8 us/key   straight (67 keys)
   6,486 keys   add 29.2   interp 27.1   tangents 61.4   =  88.5 us/key   straight (65 keys)
```

6,486 is B2's own key count, so that row is a direct comparison and not an
extrapolation.

**But the auto-bezier call is O(n²), and it is the whole cost.** Doing all three
calls as B2 does:

```
mode=full
   1,000 keys   bezier 142.9   =  228.2 us/key
   3,000 keys   bezier 359.5   =  446.8 us/key
```

The bezier phase's *per-key* price grows with the key count — 2.5× the cost for
3× the keys — so the total is quadratic. The crashed harness saw the same shape
further out: 151 µs/key at 1,000 rising to 3,197 at 12,000, by which point
native is **worse** than ExtendScript. Something inside `AEGP_SetKeyframeFlag`
walks the keyframe list on every call.

**Dropping it leaves the path straight anyway**, which is what makes the flat
number the real one. Every run above reports `straight`: interpolation LINEAR,
`SPATIAL_AUTOBEZIER` clear, and spatial tangents actually zero, on 65–67 sampled
keys spread across the range with the last always forced onto the final key.

### What this costs in the cases that decide anything

| | ExtendScript | native |
|---|---|---|
| the LINEAR pass, per key | 853 µs | 88.5 µs |
| 12,000 keys (projected) | 10 s | 1.1 s |
| fifty shards × 300 frames (projected) | 13 s | 1.3 s |

**Fracture is affordable.** It was the case that turned Wall I from an
optimisation into a requirement, and 1.3 s is not a wall.

### The part that did not go the way it was supposed to

**Native's batch add is *slower* than ExtendScript's.** `setValuesAtTimes` was
measured at 19.6 µs/key; the native `StartAddKeyframes` / `AddKeyframes` /
`SetAddKeyframe` / `EndAddKeyframes` sequence costs ~29–31. So the entire win is
in the interpolation pass, and going native buys nothing on the value writes —
which B2 had already established were free.

That is worth stating plainly because it inverts the intuition the spike started
with. The whole apply is 117.7 µs/key native against 872.6 for ExtendScript
(19.6 + 853), about 7.4× — a smaller and more honest number than the 10× on the
pass alone.

### Why the comparison is, if anything, generous to ExtendScript

B2's 853 µs/key is a **blend**. Its `makeLinear()` makes three calls per Position
key but only one per Rotation key, since rotation is not spatial and has no
tangents, and the 6,486 keys were Position *and* Rotation across three layers.
The cheap rotation keys pull that average down.

The native figure here is Position-only — the expensive stream, with tangents.
So the true ExtendScript cost for Position alone is *higher* than 853, and
native's advantage on like-for-like work is at least 10×, not at most.

### One thing measured and one thing assumed

Measured: dropping the auto-bezier call leaves the path straight. Assumed: *why*.

The obvious explanation is that `AEGP_SetKeyframeSpatialTangents` clears the flag
as a side effect. But there is a second one that fits the same evidence —
keyframes created through `AEGP_AddKeyframes` may simply never get spatial
auto-bezier in the first place, unlike the ones ExtendScript's `setValuesAtTimes`
creates. The read-back happens after the tangent phase, so it cannot tell those
apart.

It does not change the recommendation, because the product creates its keys
natively either way. It does mean the mechanism is unverified, and the cheap way
to settle it is to read the flag *before* the tangent phase rather than after.

### Scope

Measured on a scratch comp with one solid layer's Position stream,
dimensionality 3, at 1,000 / 3,000 / 6,486 keys. The 12,000-key and fracture
figures above are **projected**, not measured: the harness is capped at 6,486
after the earlier sweep exhausted memory, and the client labels them as
projections rather than quoting them as results. Verification is by sampling
65–67 keys per run, not every key.

### Two instrumentation faults worth remembering

Both produced confident, wrong headlines before being caught:

1. **Comparing one native phase against B2's three-call pass.** B2's
   `makeLinear()` sets the interpolation type, clears spatial auto-bezier and
   zeroes the tangents, and its 853 µs/key covers all three. Timing only the
   interpolation natively and dividing gave "49× faster" — a third of the work
   measured against the whole of it.
2. **`interp_readback` reports one named key, and the sampler steps past it.**
   It read 0, meaning "never sampled", on a run where all 66 sampled keys were
   clean — and 0 is indistinguishable from total failure. The client now judges
   the sampled counts and ignores that field.
