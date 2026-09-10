//! The shell's backend: the AE bridge, the solver, and what outlives the window.
//!
//! C1.1 is the Tauri window the plan has been describing since Phase C was
//! decided -- an application that lives OUTSIDE After Effects, talks to the
//! AEGP over a local pipe, and owns its own settings. C0.1 proved the
//! transport; this is the first thing built on top of it that a person uses.
//!
//! WHAT THIS LAYER IS ALLOWED TO KNOW
//! ----------------------------------
//! Almost nothing about the schema. `read_scene` hands the document to the
//! front end as the TEXT that came off the pipe, and the front end parses it.
//! That is deliberate: `ae-physics-scene/2` already has two implementations
//! that must agree -- `scene_io.py` and `b1_read_shapes.jsx` -- and the plan
//! already worries about those drifting apart. A third, in Rust, reading the
//! same fields for a layer list, would be a third place to get it wrong for no
//! gain. Rust moves bytes and runs processes here.
//!
//! The one thing it does own is WRITING THE SCENE DOWN. B3 takes a file path,
//! not a document, and B3's staleness guard (Wall K) hashes that file. Keeping
//! the scene only in the window would mean inventing a second way to feed the
//! solver, and C3's whole job is that the staleness mechanism survives the GUI.

mod bridge;
mod settings;
mod solver;
mod verify;

use settings::{Params, Paths, Settings};
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, Instant};
use tauri::Manager;

/// B1's reader, run inside AE by the bridge.
const READER: &str = "b1_read_shapes.jsx";

/// A read runs `AEGP_ExecuteScript` on AE's UI thread. C0.1 measured the whole
/// round trip at 16 ms on a three-layer comp, so this is not a time budget --
/// it is the point at which "AE has a modal open" stops looking like a hang.
const READ_TIMEOUT: Duration = Duration::from_secs(120);
const PING_TIMEOUT: Duration = Duration::from_secs(5);
/// An apply writes thousands of keyframes on AE's UI thread and AE is not
/// repainting while it does. C0.3 projected 12,000 keys at about 1.1 s
/// natively; this is the point at which something is wrong, not a budget.
const APPLY_TIMEOUT: Duration = Duration::from_secs(300);

struct App {
    settings: Mutex<Settings>,
}

#[derive(serde::Serialize)]
struct Reply {
    ok: bool,
    /// The document, verbatim off the pipe. Parsed by the front end.
    text: String,
    /// Where it was written, which is what the solver is given.
    path: String,
    bytes: usize,
    ms: u128,
}

/// What an apply came back with, or why it did not happen.
#[derive(serde::Serialize)]
struct ApplyReply {
    ok: bool,
    /// The apply did not run because the comp no longer matches the bake.
    /// Not a failure: the guard worked.
    stale: bool,
    verdict: Option<verify::Verdict>,
    /// The AEGP's own tally, verbatim -- key counts and phase timings.
    reply: String,
    ms: u128,
}

/// Geometry and keyframes, verbatim. Parsed by the front end, never here.
#[derive(serde::Serialize)]
struct Viewport {
    render: String,
    bake: String,
}

fn work_dir(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("no data directory: {e}"))?
        .join("work");
    std::fs::create_dir_all(&dir)
        .map_err(|e| format!("could not create {}: {e}", dir.display()))?;
    Ok(dir)
}

/// Ask AE for the active comp, and return the document EXACTLY as it arrived.
///
/// Shared by `read_scene` and `verify_bake` on purpose: Wall K's guard compares
/// a hash of these bytes against the hash B3 recorded, so the two paths must
/// ask the same question and keep the same answer. Re-serialising here would
/// re-order keys and re-format floats and break the comparison silently.
async fn read_scene_bytes(paths: &Paths) -> Result<String, String> {
    let script = paths.script(READER);
    if !script.exists() {
        return Err(format!(
            "{} is not in the prototype folder.

Set the folder in              Settings -- it is the python-proto/physics_sim checkout, holding              {READER} and {}.",
            script.display(),
            solver::SOLVER
        ));
    }
    let payload = serde_json::json!({
        "cmd": "read_scene",
        "script": script.display().to_string(),
    })
    .to_string();

    let text =
        tauri::async_runtime::spawn_blocking(move || bridge::request(&payload, READ_TIMEOUT))
            .await
            .map_err(|e| format!("the read task did not finish: {e}"))??;

    // The reader reports its own failures as a JSON error rather than a modal,
    // because a modal raised from the AEGP's idle hook would block AE waiting
    // for a click nobody is there to give. So a well-formed refusal arrives
    // here looking exactly like a document, and has to be told apart.
    let parsed: serde_json::Value = serde_json::from_str(&text).map_err(|e| {
        format!(
            "the bridge replied with something that is not JSON ({e}):

{}",
            text.chars().take(400).collect::<String>()
        )
    })?;
    if parsed.get("ok") == Some(&serde_json::Value::Bool(false)) {
        return Err(parsed
            .get("error")
            .and_then(|v| v.as_str())
            .unwrap_or("the bridge refused the request and did not say why")
            .to_string());
    }
    Ok(text)
}

/// Apply the bake through the AEGP, natively.
///
/// C0.3 measured the native keyframe path at 117.7 us/key against
/// ExtendScript's 872.6 and then left it unused while the product paid the
/// slow one. This is the wiring, and `b2_apply_bake.jsx` stays exactly where
/// it is -- it is the reference implementation every one of those numbers is a
/// comparison against, and a second implementation you can no longer run is a
/// second implementation you can no longer check.
///
/// Wall K is checked FIRST, here rather than in the front end, so that a
/// caller cannot apply a stale bake by forgetting to look. `force` is how the
/// person overrides it, because "I moved a layer I did not simulate" is a real
/// and legitimate thing to have done.
#[tauri::command]
async fn apply_bake(
    app: tauri::AppHandle,
    state: tauri::State<'_, App>,
    force: bool,
) -> Result<ApplyReply, String> {
    let paths = state.settings.lock().unwrap().paths.clone();
    let bake_path = work_dir(&app)?.join("bake.json");
    if !bake_path.exists() {
        return Err(format!(
            "there is no bake to apply ({} does not exist) -- run the solver \
             first.",
            bake_path.display()
        ));
    }

    let verdict = if force {
        None
    } else {
        let raw = std::fs::read(&bake_path)
            .map_err(|e| format!("could not read {}: {e}", bake_path.display()))?;
        let bake: serde_json::Value = serde_json::from_slice(&raw)
            .map_err(|e| format!("{} is not JSON ({e})", bake_path.display()))?;
        let live = read_scene_bytes(&paths).await?;
        let v = verify::compare(&bake, live.as_bytes())?;
        if !v.fresh {
            // Not an error in the sense of something going wrong -- the check
            // did its job. The front end turns this into a question.
            return Ok(ApplyReply {
                ok: false,
                stale: true,
                verdict: Some(v),
                reply: String::new(),
                ms: 0,
            });
        }
        Some(v)
    };

    let payload = serde_json::json!({
        "cmd": "apply_bake",
        "script": bake_path.display().to_string(),
    })
    .to_string();

    let t0 = Instant::now();
    let reply =
        tauri::async_runtime::spawn_blocking(move || bridge::request(&payload, APPLY_TIMEOUT))
            .await
            .map_err(|e| format!("the apply task did not finish: {e}"))??;

    let parsed: serde_json::Value = serde_json::from_str(&reply)
        .map_err(|e| format!("the bridge replied with something that is not JSON ({e}):\n\n{reply}"))?;
    if parsed.get("ok") == Some(&serde_json::Value::Bool(false)) {
        return Err(parsed
            .get("error")
            .and_then(|v| v.as_str())
            .unwrap_or("the bridge refused the apply and did not say why")
            .to_string());
    }

    Ok(ApplyReply {
        ok: true,
        stale: false,
        verdict,
        reply,
        ms: t0.elapsed().as_millis(),
    })
}

/// The two documents the viewport draws from, as TEXT.
///
/// Rust does not parse either one. C1.1's rule holds: `ae-physics-scene` and
/// `ae-physics-bake` already have two implementations that must agree, and a
/// third in Rust reading the same fields would be a third place to drift. The
/// front end owns the TRANSFORM -- `position + R(theta) * (v - anchor)` and
/// linear sampling -- which is the one part a canvas cannot delegate, and
/// `c2_render_model.py` checks that arithmetic against `preview.py` at
/// fractional frames, where A5 says the damage hides.
#[tauri::command]
fn load_viewport(app: tauri::AppHandle) -> Result<Viewport, String> {
    let dir = work_dir(&app)?;
    let render = dir.join("render.json");
    let bake = dir.join("bake.json");

    if !render.exists() || !bake.exists() {
        return Err("there is nothing to look at yet -- run the solver first."
            .into());
    }
    Ok(Viewport {
        render: std::fs::read_to_string(&render)
            .map_err(|e| format!("could not read {}: {e}", render.display()))?,
        bake: std::fs::read_to_string(&bake)
            .map_err(|e| format!("could not read {}: {e}", bake.display()))?,
    })
}

/// Wall K: is this bake still the answer to the comp that is open NOW?
///
/// Re-reads rather than trusting the bake's own `source` block -- see
/// `verify.rs` for why the block cannot be trusted and what the apply script
/// is structurally unable to see.
#[tauri::command]
async fn verify_bake(
    app: tauri::AppHandle,
    state: tauri::State<'_, App>,
) -> Result<verify::Verdict, String> {
    let paths = state.settings.lock().unwrap().paths.clone();
    let bake_path = work_dir(&app)?.join("bake.json");
    if !bake_path.exists() {
        return Err(format!(
            "there is no bake to check yet ({} does not exist) -- run the              solver first.",
            bake_path.display()
        ));
    }
    let raw = std::fs::read(&bake_path)
        .map_err(|e| format!("could not read {}: {e}", bake_path.display()))?;
    let bake: serde_json::Value = serde_json::from_slice(&raw)
        .map_err(|e| format!("{} is not JSON ({e})", bake_path.display()))?;

    let live = read_scene_bytes(&paths).await?;
    verify::compare(&bake, live.as_bytes())
}

/// The bridge answers `{"ok":true,"pong":true}`, and that is the whole check:
/// AE is up, the plug-in loaded, and the pipe round-trips.
#[tauri::command]
async fn bridge_ping() -> Result<String, String> {
    let t0 = Instant::now();
    let reply = tauri::async_runtime::spawn_blocking(move || {
        bridge::request(r#"{"cmd":"ping"}"#, PING_TIMEOUT)
    })
    .await
    .map_err(|e| format!("the ping task did not finish: {e}"))??;
    Ok(format!(
        "{reply}   ({} ms round trip)",
        t0.elapsed().as_millis()
    ))
}

#[tauri::command]
async fn read_scene(app: tauri::AppHandle, state: tauri::State<'_, App>) -> Result<Reply, String> {
    let paths = state.settings.lock().unwrap().paths.clone();
    let t0 = Instant::now();
    let text = read_scene_bytes(&paths).await?;
    let ms = t0.elapsed().as_millis();

    let path = work_dir(&app)?.join("scene.json");
    // Written with the bytes that arrived, not re-serialised. C0.1's pass
    // criterion is byte-identity with what the save dialog writes, and a
    // round trip through a JSON library here would quietly re-order keys and
    // re-format floats -- destroying the one property that was verified, and
    // with it the hash Wall K's guard compares against.
    std::fs::write(&path, text.as_bytes())
        .map_err(|e| format!("could not write {}: {e}", path.display()))?;

    Ok(Reply {
        ok: true,
        bytes: text.len(),
        text,
        path: path.display().to_string(),
        ms,
    })
}

#[tauri::command]
async fn simulate(
    app: tauri::AppHandle,
    state: tauri::State<'_, App>,
) -> Result<solver::SolveResult, String> {
    let (paths, params) = {
        let s = state.settings.lock().unwrap();
        (s.paths.clone(), s.params.clone())
    };
    let dir = work_dir(&app)?;
    let scene = dir.join("scene.json");
    if !scene.exists() {
        return Err("no scene has been read yet -- read the comp from After \
                    Effects first."
            .into());
    }

    tauri::async_runtime::spawn_blocking(move || solver::solve(&paths, &params, &scene, &dir))
        .await
        .map_err(|e| format!("the solver task did not finish: {e}"))?
}

#[tauri::command]
fn get_settings(state: tauri::State<'_, App>) -> Settings {
    state.settings.lock().unwrap().clone()
}

/// Saved on every change rather than on a Save button.
///
/// The settings a user notices are the parameters, and they change constantly;
/// a Save button for those is a way to lose a session's tuning by closing the
/// window. The write is small and local, so there is nothing to batch for.
#[tauri::command]
fn set_settings(
    app: tauri::AppHandle,
    state: tauri::State<'_, App>,
    paths: Paths,
    params: Params,
) -> Result<(), String> {
    let mut s = state.settings.lock().unwrap();
    s.paths = paths;
    s.params = params;
    settings::save(&app, &s)
}

/// The window has no console, and a desktop app that fails silently is worse
/// than one that fails loudly. The front end reports what it could not do here
/// so it reaches stderr and the terminal that launched the app -- which is how
/// this file's own first bug was found: `boot()` threw, nothing pinged, and the
/// window sat there looking finished.
#[tauri::command]
fn log_js(message: String) {
    eprintln!("[ui] {message}");
}

#[tauri::command]
fn settings_path(app: tauri::AppHandle) -> String {
    settings::location(&app)
}

/// "Is this folder the prototype, and does that interpreter have pymunk?"
#[tauri::command]
async fn probe_paths(paths: Paths) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || solver::probe(&paths))
        .await
        .map_err(|e| format!("the probe task did not finish: {e}"))?
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let loaded = settings::load(app.handle());
            app.manage(App {
                settings: Mutex::new(loaded),
            });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            bridge_ping,
            read_scene,
            simulate,
            get_settings,
            set_settings,
            settings_path,
            log_js,
            probe_paths,
            verify_bake,
            apply_bake,
            load_viewport
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
