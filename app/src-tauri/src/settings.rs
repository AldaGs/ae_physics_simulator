//! What outlives the window.
//!
//! The plan's argument for an application rather than a panel is partly this:
//! "a record pool, a scene library and settings that outlive a comp are natural
//! in an application and awkward in a panel". So settings are not a convenience
//! here, they are one of the reasons the shell exists.
//!
//! Two kinds of thing live in one file, and the split matters:
//!
//!   `paths`   where Python and the prototype are. Machine-shaped, set once,
//!             and the app is useless until they are right.
//!   `params`  B3's parameter set. Per-session, changed constantly, and the
//!             whole point of having controls.
//!
//! They are saved together because there is one document and one save, and
//! separated in the shape because a future "reset parameters" must not offer to
//! forget where Python is.
//!
//! WHY THE DEFAULTS ARE B3'S DEFAULTS
//! ----------------------------------
//! Every number below is the one `b3_loop.py --help` already prints, and they
//! are not repeated for tidiness: `ppm = 100` is A1's measured usable band
//! (10-1000) settled on, `substeps = 8` is what makes frames land on substep
//! boundaries so nothing is ever interpolated, and `gravity = 9.8` is the one
//! the whole of Phase A was verified against. A control that starts somewhere
//! else would quietly invalidate the comparison every stored bake was made
//! under.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Paths {
    /// The interpreter. A bare name is resolved on PATH, which is what makes
    /// "python" a workable default; an absolute path pins a venv.
    pub python: String,
    /// The `python-proto/physics_sim` checkout: `b3_loop.py` and
    /// `b1_read_shapes.jsx` both live here.
    pub proto_dir: String,
}

impl Default for Paths {
    fn default() -> Self {
        Self {
            python: "python".into(),
            proto_dir: String::new(),
        }
    }
}

impl Paths {
    pub fn script(&self, name: &str) -> PathBuf {
        PathBuf::from(&self.proto_dir).join(name)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Params {
    pub gravity: f64,
    pub ppm: f64,
    pub substeps: u32,
    /// None means "the comp's own duration", which is what B3 does with no
    /// `--frames`. Zero would have been the tempting encoding and is wrong:
    /// zero frames is a real, if useless, request.
    pub frames: Option<u32>,
    pub friction: f64,
    pub elasticity: f64,
    /// Layer ids to pin. B3 takes `--static` repeatedly.
    ///
    /// IDS, not names: AE allows duplicate layer names, so a name does not
    /// identify a layer. The front end migrates settings written before that
    /// was fixed.
    pub statics: Vec<String>,
    /// Per-layer physics, keyed by layer id, over the scene-wide values above.
    ///
    /// The solver was ALWAYS per-body -- `sim.PolyBody` carries density,
    /// friction and elasticity on every spec. What made a scene uniform was
    /// b3_loop stamping the globals over all of them. So these are not a new
    /// capability, they are the existing one becoming reachable.
    ///
    /// Absent is not zero. A layer with no entry inherits the scene value, and
    /// a layer with `friction: 0.0` was deliberately made frictionless -- the
    /// same distinction `frames: None` draws against `frames: 0`.
    pub layer_params: std::collections::BTreeMap<String, LayerParams>,
    pub no_walls: bool,
    pub allow_escapes: bool,
}

/// One layer's overrides. Every field optional, because an unset control has
/// to be distinguishable from one set to zero.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct LayerParams {
    /// Kilograms. B3 back-solves density from the layer's own area, because
    /// mass = density * area and only the geometry side knows the area.
    pub mass: Option<f64>,
    pub friction: Option<f64>,
    pub bounce: Option<f64>,
}

impl LayerParams {
    pub fn is_empty(&self) -> bool {
        self.mass.is_none() && self.friction.is_none() && self.bounce.is_none()
    }
}

impl Default for Params {
    fn default() -> Self {
        Self {
            gravity: 9.8,
            ppm: 100.0,
            substeps: 8,
            frames: None,
            friction: 0.6,
            elasticity: 0.2,
            statics: Vec::new(),
            layer_params: std::collections::BTreeMap::new(),
            no_walls: false,
            allow_escapes: false,
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub paths: Paths,
    pub params: Params,
}

fn file(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    use tauri::Manager;
    let dir = app
        .path()
        .app_config_dir()
        .map_err(|e| format!("no config directory: {e}"))?;
    std::fs::create_dir_all(&dir)
        .map_err(|e| format!("could not create {}: {e}", dir.display()))?;
    Ok(dir.join("settings.json"))
}

/// Missing or unreadable settings are DEFAULTS, never an error.
///
/// The first run has no file, and a file half-written by a crash is the same
/// situation as no file: the app has to open. A corrupt settings file that
/// refuses to launch the window is a worse outcome than losing a gravity value,
/// so the read is deliberately forgiving -- and the write below is deliberately
/// not, because a save that silently does nothing is how settings "stop
/// persisting" with nothing to explain it.
pub fn load(app: &tauri::AppHandle) -> Settings {
    let Ok(path) = file(app) else {
        return Settings::default();
    };
    let Ok(text) = std::fs::read_to_string(&path) else {
        return Settings::default();
    };
    serde_json::from_str(&text).unwrap_or_else(|e| {
        eprintln!("{}: ignoring unreadable settings ({e})", path.display());
        Settings::default()
    })
}

pub fn save(app: &tauri::AppHandle, s: &Settings) -> Result<(), String> {
    let path = file(app)?;
    let text = serde_json::to_string_pretty(s)
        .map_err(|e| format!("could not serialise settings: {e}"))?;
    std::fs::write(&path, text)
        .map_err(|e| format!("could not write {}: {e}", path.display()))
}

pub fn location(app: &tauri::AppHandle) -> String {
    file(app)
        .map(|p| p.display().to_string())
        .unwrap_or_else(|e| e)
}
