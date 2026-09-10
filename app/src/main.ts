/**
 * The shell's front end.
 *
 * WHAT LIVES HERE AND WHAT DOES NOT
 * ---------------------------------
 * The scene document is parsed HERE, not in Rust. `ae-physics-scene/2` already
 * has two implementations that have to agree -- `scene_io.py` and
 * `b1_read_shapes.jsx` -- and the plan already worries about those drifting.
 * A third one in Rust, reading the same fields to build a list, would be a
 * third place to get it wrong for nothing. Rust moves bytes and runs processes.
 *
 * What this file reads out of the document is deliberately shallow: comp facts,
 * layer names and ids, path counts, warnings. It never touches geometry and it
 * never writes a scene. If it ever needs to, that is a real schema question and
 * belongs in `scene_io`, which has the validator.
 *
 * PINNING IS THE ONE PLACE THE TWO PANELS MEET
 * --------------------------------------------
 * Clicking a layer adds its NAME to `params.statics`, which becomes B3's
 * `--static`. That is why the scene list is not decorative: it is the control
 * surface for the one parameter that is per-layer rather than per-scene.
 *
 * By name and not by id, even though `id` is the join key everywhere else in
 * this project. B3 accepts either, and a name survives the re-read that C3 is
 * going to add -- whereas an id is positional, and B2 already learned what that
 * costs: adding or reordering a layer between the read and the apply silently
 * moves every id after it.
 */

import { invoke } from "@tauri-apps/api/core";

// --------------------------------------------------------------------------
// The shapes we read out of the two documents. Partial on purpose: see above.
// --------------------------------------------------------------------------

type Layer = {
  id: number;
  name: string;
  paths?: unknown[];
  motion?: string;
  scale_animated?: boolean;
};

type Scene = {
  schema: string;
  comp: { name?: string; width: number; height: number; fps: number; duration_frames: number };
  layers: Layer[];
  warnings?: string[];
};

type Paths = { python: string; proto_dir: string };

type Params = {
  gravity: number;
  ppm: number;
  substeps: number;
  frames: number | null;
  friction: number;
  elasticity: number;
  statics: string[];
  no_walls: boolean;
  allow_escapes: boolean;
};

type Settings = { paths: Paths; params: Params };

type ReadReply = { ok: boolean; text: string; path: string; bytes: number; ms: number };

/** Wall K's answer. `fresh` is the hash comparison; `reasons` is only what
 *  the apply script would also have caught, so an empty list with fresh=false
 *  means the change is geometric. */
type Verdict = {
  fresh: boolean;
  reasons: string[];
  bake_sha256: string;
  live_sha256: string;
  made_at: string;
  bake_comp: string;
  live_comp: string;
};

/** The AEGP's own tally, parsed only to show it. */
type ApplyReply = {
  ok: boolean;
  stale: boolean;
  verdict: Verdict | null;
  reply: string;
  ms: number;
};

type SolveResult = {
  ok: boolean;
  refused: boolean;
  bake_path: string;
  preview_path: string;
  stdout: string;
  stderr: string;
  exit_code: number | null;
  command: string;
};

// --------------------------------------------------------------------------

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

let settings: Settings;
let scene: Scene | null = null;

/**
 * Settings are saved on every change rather than behind a Save button.
 *
 * The settings a user actually notices are the parameters, and those change
 * constantly while tuning; a Save button for them is a way to lose a session's
 * work by closing the window. The debounce is only to keep a dragged number
 * input from writing the file on every tick.
 */
let saveTimer: number | undefined;
function persist() {
  window.clearTimeout(saveTimer);
  saveTimer = window.setTimeout(() => {
    invoke("set_settings", { paths: settings.paths, params: settings.params }).catch((e) =>
      console.error("could not save settings:", e),
    );
  }, 250);
}

function fail(message: string, where: HTMLElement) {
  where.hidden = false;
  where.className = "bad";
  where.textContent = String(message);
}

// --------------------------------------------------------------------------
// The bridge
// --------------------------------------------------------------------------

function setBridge(state: "up" | "down" | "unknown" | "busy", text: string) {
  $("bridge-dot").dataset.state = state;
  $("bridge-text").textContent = text;
}

async function ping() {
  setBridge("busy", "checking…");
  try {
    setBridge("up", await invoke<string>("bridge_ping"));
  } catch (e) {
    // The message from Rust already says what to do about it; it is long
    // because "the system cannot find the file specified" is not a diagnosis.
    setBridge("down", "no bridge");
    alert(e);
  }
}

// --------------------------------------------------------------------------
// The scene
// --------------------------------------------------------------------------

function renderScene() {
  const body = $("scene-body");
  const empty = $("scene-empty");
  if (!scene) {
    body.hidden = true;
    empty.hidden = false;
    return;
  }
  empty.hidden = true;
  body.hidden = false;

  const c = scene.comp;
  $("comp-facts").innerHTML = [
    ["comp", c.name || "(unnamed)"],
    ["size", `${c.width} × ${c.height}`],
    ["rate", `${c.fps} fps, ${c.duration_frames} frames`],
    ["schema", scene.schema],
  ]
    .map(([k, v]) => `<dt>${k}</dt><dd>${esc(String(v))}</dd>`)
    .join("");

  const pinned = new Set(settings.params.statics);
  $("layers").innerHTML = scene.layers
    .map((l) => {
      const isPinned = pinned.has(l.name) || pinned.has(String(l.id));
      const notes: string[] = [];
      if (l.paths) notes.push(`${l.paths.length} path${l.paths.length === 1 ? "" : "s"}`);
      if (l.scale_animated) notes.push("animated scale");
      if (l.motion && l.motion !== "dynamic") notes.push(l.motion);
      return `<li class="layer${isPinned ? " pinned" : ""}" data-name="${esc(l.name)}">
        <span class="pin">${isPinned ? "pinned" : ""}</span>
        <span class="name">${esc(l.name)}</span>
        <span class="meta">id ${l.id}${notes.length ? " · " + esc(notes.join(" · ")) : ""}</span>
      </li>`;
    })
    .join("");

  for (const el of Array.from($("layers").children)) {
    el.addEventListener("click", () => togglePin((el as HTMLElement).dataset.name!));
  }

  const w = scene.warnings ?? [];
  $("warnings").innerHTML = w.length
    ? `<h3>${w.length} warning${w.length === 1 ? "" : "s"} from the reader</h3><ul>` +
      w.map((s) => `<li>${esc(s)}</li>`).join("") +
      "</ul>"
    : "";
}

function togglePin(name: string) {
  const at = settings.params.statics.indexOf(name);
  if (at >= 0) settings.params.statics.splice(at, 1);
  else settings.params.statics.push(name);
  persist();
  renderScene();
}

async function readScene() {
  const btn = $<HTMLButtonElement>("read");
  btn.disabled = true;
  btn.textContent = "reading…";
  const result = $("result");
  result.hidden = true;
  try {
    const reply = await invoke<ReadReply>("read_scene");
    scene = JSON.parse(reply.text) as Scene;

    // Pinning a layer that is no longer in the comp would silently do nothing,
    // and B3 would not complain: --static takes a name it may not find. So the
    // list is reconciled against what actually came back.
    const present = new Set<string>();
    for (const l of scene.layers) {
      present.add(l.name);
      present.add(String(l.id));
    }
    const dropped = settings.params.statics.filter((s) => !present.has(s));
    if (dropped.length) {
      settings.params.statics = settings.params.statics.filter((s) => present.has(s));
      persist();
      alert(
        `These pinned layers are not in the comp any more, so they have been ` +
          `un-pinned:\n\n  ${dropped.join("\n  ")}\n\n` +
          `A pin that names a layer that is not there does nothing, and the ` +
          `solver would not have said so.`,
      );
    }

    renderScene();
    $<HTMLButtonElement>("simulate").disabled = false;
    setBridge("up", `read ${scene.layers.length} layers, ${reply.bytes} bytes in ${reply.ms} ms`);
  } catch (e) {
    alert(e);
  } finally {
    btn.disabled = false;
    btn.textContent = "Read comp from AE";
  }
}

// --------------------------------------------------------------------------
// The solver
// --------------------------------------------------------------------------

async function simulate() {
  const btn = $<HTMLButtonElement>("simulate");
  const result = $("result");
  btn.disabled = true;
  btn.textContent = "simulating…";
  result.hidden = false;
  result.className = "";
  result.textContent = "running the solver…";
  try {
    const r = await invoke<SolveResult>("simulate");
    if (r.ok) {
      result.className = "good";
      result.innerHTML =
        `<h3>Bake written</h3><pre>${esc(r.stdout.trim())}</pre>` +
        `<p class="hint">${esc(r.bake_path)}</p>` +
        `<p class="hint">Apply it in AE with b2_apply_bake.jsx. The viewport ` +
        `that lets you check it before applying is C2 — until then the ` +
        `preview PNG is the only look you get, and A5 is the argument for why ` +
        `looking matters.</p>`;
    } else if (r.refused) {
      // Not a failure. B3 finished, and the answer is no.
      result.className = "warn";
      result.innerHTML =
        `<h3>Refused: a layer left the comp</h3><pre>${esc(
          (r.stderr || r.stdout).trim(),
        )}</pre><p class="hint">Nothing was written. Enclose the comp, or ` +
        `tick “write the bake even if a layer escapes” if you meant it.</p>`;
    } else {
      result.className = "bad";
      result.innerHTML =
        `<h3>The solver failed (exit ${r.exit_code ?? "?"})</h3><pre>${esc(
          (r.stderr || r.stdout).trim(),
        )}</pre><p class="hint">${esc(r.command)}</p>`;
    }
  } catch (e) {
    fail(String(e), result);
  } finally {
    btn.disabled = false;
    btn.textContent = "Simulate";
  }
}


/**
 * Wall K, asked the only way that can answer it: read the comp AGAIN.
 *
 * The bake's `source` block describes the scene; `b2_apply_bake.jsx` can only
 * see the comp; names and dimensions are the entire overlap. Measured
 * 2026-09-10: a nudged Position applies anyway and the nudge is overwritten
 * from keyframe 0. So the shell does what the script structurally cannot.
 *
 * A stale bake is REPORTED, not blocked. Re-reading after moving a layer you
 * did not simulate is a legitimate thing to do, and "this was computed from
 * different geometry" is the honest sentence -- not "you may not".
 */
async function verifyBake() {
  const btn = $<HTMLButtonElement>("verify");
  const box = $("verify-result");
  btn.disabled = true;
  btn.textContent = "reading comp…";
  box.hidden = false;
  box.className = "";
  box.textContent = "asking AE for the comp as it is now…";
  try {
    const v = await invoke<Verdict>("verify_bake");
    const made = v.made_at ? ` (baked ${esc(v.made_at)})` : "";
    if (v.fresh) {
      box.className = "good";
      box.innerHTML =
        `<h3>The bake matches the comp</h3>` +
        `<p>Comp <b>${esc(v.live_comp)}</b>${made} hashes identically to the ` +
        `scene this bake was computed from.</p>` +
        `<p class="hint">sha256 ${esc(v.bake_sha256.slice(0, 16))}…</p>`;
    } else {
      box.className = "warn";
      const named = v.reasons.length
        ? `<ul>${v.reasons.map((r) => `<li>${esc(r)}</li>`).join("")}</ul>`
        : `<p>Nothing in the comp's <i>identity</i> changed — same name, same ` +
          `size, same layers. The difference is geometric: a position, a ` +
          `rotation, a scale or a path is not what it was when this was ` +
          `simulated. That is the case the apply script cannot see.</p>`;
      box.innerHTML =
        `<h3>Stale: this bake was computed from different geometry</h3>` +
        named +
        `<p class="hint">bake ${esc(v.bake_sha256.slice(0, 16))}… vs comp ` +
        `${esc(v.live_sha256.slice(0, 16))}…${made}</p>` +
        `<p class="hint">Applying it will overwrite the comp with keyframes ` +
        `computed from geometry that no longer exists. Re-read and simulate ` +
        `again, unless you meant it.</p>`;
    }
  } catch (e) {
    fail(String(e), box);
  } finally {
    btn.disabled = false;
    btn.textContent = "Check bake against AE";
  }
}


/**
 * Apply, with Wall K in front of it.
 *
 * The staleness check runs in the BACKEND, before the bridge is touched, so it
 * cannot be bypassed by a front end that forgot to look. A stale bake comes
 * back as `stale: true` rather than an error, because the guard doing its job
 * is not a failure - and the answer to it is a question rather than a refusal:
 * re-reading after moving a layer you did not simulate is legitimate, and only
 * the person knows which it was.
 */
async function applyBake(force = false) {
  const btn = $<HTMLButtonElement>("apply");
  const box = $("verify-result");
  btn.disabled = true;
  btn.textContent = force ? "applying..." : "checking...";
  box.hidden = false;
  box.className = "";
  box.textContent = force
    ? "writing keyframes..."
    : "checking the comp, then writing keyframes...";
  try {
    const r = await invoke<ApplyReply>("apply_bake", { force });

    if (r.stale && r.verdict) {
      const v = r.verdict;
      const named = v.reasons.length
        ? `<ul>${v.reasons.map((x) => `<li>${esc(x)}</li>`).join("")}</ul>`
        : `<p>Same comp, same layers, same names \u2014 the difference is ` +
          `geometric. Something moved since this was simulated.</p>`;
      box.className = "warn";
      box.innerHTML =
        `<h3>Nothing was written: the comp has changed</h3>` +
        named +
        `<p class="hint">bake ${esc(v.bake_sha256.slice(0, 16))}\u2026 vs comp ` +
        `${esc(v.live_sha256.slice(0, 16))}\u2026</p>`;
      const again = document.createElement("button");
      again.className = "ghost";
      again.textContent = "Apply anyway";
      again.addEventListener("click", () => applyBake(true));
      box.appendChild(again);
      return;
    }

    const t = JSON.parse(r.reply || "{}");
    const perKey = typeof t.us_per_key === "number" ? t.us_per_key : null;
    // C0.3's read-back, surfaced rather than buried: if AE ever stops clearing
    // spatial auto-bezier by itself, every motion path between keyframes bows
    // and no still frame shows it. A5 spent a whole phase on that failure.
    const bezier =
      t.autobezier_still_set > 0
        ? `<p class="hint"><b>Warning:</b> spatial auto-bezier is still set on ` +
          `${esc(String(t.autobezier_still_set))} of ` +
          `${esc(String(t.autobezier_sampled))} sampled keys. The motion path ` +
          `bows between keyframes. C0.3 measured this pass as unnecessary \u2014 ` +
          `if you are seeing this, that assumption has stopped holding.</p>`
        : "";
    box.className = "good";
    box.innerHTML =
      `<h3>Applied</h3>` +
      `<p>${esc(String(t.keys ?? "?"))} keyframes across ` +
      `${esc(String(t.layers ?? "?"))} layers in ` +
      `${esc(String(Math.round(t.ms ?? r.ms)))} ms` +
      (perKey ? ` \u2014 ${esc(perKey.toFixed(1))} \u00b5s/key` : "") +
      `.</p>` +
      (t.skipped_static
        ? `<p class="hint">${esc(String(t.skipped_static))} pinned layer(s) ` +
          `got no keyframes at all, which is B3's rule: writing even a ` +
          `constant would overwrite your own placement.</p>`
        : "") +
      bezier +
      `<p class="hint">One Undo puts the comp back.</p>`;
  } catch (e) {
    fail(String(e), box);
  } finally {
    btn.disabled = false;
    btn.textContent = "Apply to AE";
  }
}

// --------------------------------------------------------------------------
// Controls
// --------------------------------------------------------------------------

function esc(s: string) {
  const d = document.createElement("div");
  d.textContent = s;
  return d.innerHTML;
}

/** One numeric control, bound both ways. */
function num(id: string, key: keyof Params, parse: (s: string) => number | null) {
  const el = $<HTMLInputElement>(id);
  const v = settings.params[key];
  el.value = v === null || v === undefined ? "" : String(v);
  el.addEventListener("input", () => {
    const parsed = parse(el.value);
    // A half-typed number is not a value to save. Leaving the last good one in
    // place means the file never holds NaN, which reads back as a broken
    // settings file and silently resets everything.
    if (parsed === null && el.value.trim() !== "") return;
    (settings.params[key] as unknown) = parsed;
    persist();
  });
}

function bool(id: string, key: "no_walls" | "allow_escapes") {
  const el = $<HTMLInputElement>(id);
  el.checked = settings.params[key];
  el.addEventListener("change", () => {
    settings.params[key] = el.checked;
    persist();
  });
}

function text(id: string, key: keyof Paths) {
  const el = $<HTMLInputElement>(id);
  el.value = settings.paths[key];
  el.addEventListener("input", () => {
    settings.paths[key] = el.value;
    persist();
  });
}

const float = (s: string) => {
  const n = Number.parseFloat(s);
  return Number.isFinite(n) ? n : null;
};
const int = (s: string) => {
  const n = Number.parseInt(s, 10);
  return Number.isFinite(n) ? n : null;
};

async function boot() {
  settings = await invoke<Settings>("get_settings");

  num("gravity", "gravity", float);
  num("ppm", "ppm", float);
  num("substeps", "substeps", int);
  num("frames", "frames", int); // blank -> null -> B3 uses the comp's duration
  num("friction", "friction", float);
  num("elasticity", "elasticity", float);
  bool("no-walls", "no_walls");
  bool("allow-escapes", "allow_escapes");
  text("python", "python");
  text("proto-dir", "proto_dir");

  $("ping").addEventListener("click", ping);
  $("read").addEventListener("click", readScene);
  $("simulate").addEventListener("click", simulate);
  $("verify").addEventListener("click", verifyBake);
  $("apply").addEventListener("click", () => applyBake(false));
  $("probe").addEventListener("click", async () => {
    const out = $("probe-result");
    out.className = "";
    out.textContent = "checking…";
    try {
      out.textContent = await invoke<string>("probe_paths", { paths: settings.paths });
      out.className = "good-text";
    } catch (e) {
      out.textContent = String(e);
      out.className = "bad-text";
    }
  });

  $("settings-where").textContent = "Saved to " + (await invoke<string>("settings_path"));

  // The settings section opens itself when it is the thing standing in the
  // way, which on a first run it always is.
  if (!settings.paths.proto_dir) {
    $<HTMLDetailsElement>("settings-panel").querySelector("details")!.open = true;
  }

  renderScene();
  ping();
}

/** Nothing in a webview reports itself. Everything unhandled goes to Rust. */
function report(what: string, e: unknown) {
  const detail = e instanceof Error ? `${e.message}
${e.stack ?? ""}` : String(e);
  invoke("log_js", { message: `${what}: ${detail}` }).catch(() => {});
  document.body.insertAdjacentHTML(
    "afterbegin",
    `<div class="bad" style="padding:10px 18px">${esc(what)}: ${esc(detail)}</div>`,
  );
}

window.addEventListener("error", (ev) => report("uncaught", ev.error ?? ev.message));
window.addEventListener("unhandledrejection", (ev) => report("unhandled rejection", ev.reason));

boot().catch((e) => report("the window could not start", e));
