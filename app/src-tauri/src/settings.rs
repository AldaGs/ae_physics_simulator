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

/// How the app behaves, as opposed to what it simulates.
///
/// A third section rather than more fields on `params`, for the same reason
/// `paths` is separate: every one of those is an argument to `b3_loop.py` and
/// nothing else, and a preference about window management is not an argument
/// to anything.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Behaviour {
    /// After a successful apply: "stay", "minimise" or "close".
    ///
    /// The keyframes are in the project at that point, so the place to look is
    /// AE. Minimise is the default rather than close: closing throws away the
    /// viewport, the layer list and the parameters that produced the bake, and
    /// wanting a second look at what you just applied is not an unusual thing.
    pub after_apply: String,
    /// Read the comp on launch instead of waiting for the button.
    ///
    /// Reading is free and safe. It deliberately does NOT auto-simulate:
    /// simulating writes a bake over the last one, and doing that to somebody
    /// on launch is a way to lose work they had not applied yet.
    pub autoload_scene: bool,
}

impl Default for Behaviour {
    fn default() -> Self {
        Self {
            after_apply: "minimise".into(),
            autoload_scene: false,
        }
    }
}

// --------------------------------------------------------------------------
// C7.1 -- a setup belongs to a comp, not to the application
// --------------------------------------------------------------------------

/// Which comp the app is looking at, as AE itself reports it.
///
/// EVERY FIELD HERE COMES OFF THE BRIDGE, NOT OUT OF THE SCENE DOCUMENT, and
/// that is the whole design. The roadmap's C7 section names the trap: Wall K
/// hashes the WHOLE raw scene document (`verify.rs`), so a project path put
/// inside those bytes makes a Save As between simulate and apply refuse a bake
/// that is still physically valid. Identity has to live OUTSIDE the hashed
/// bytes -- and a separate bridge command is outside them by construction,
/// which no field placed in the document can be.
///
/// It also costs the reader nothing. `b1_read_shapes.jsx` keeps emitting
/// exactly the `ae-physics-scene/2` it emits today, so every scene captured
/// before this existed still hashes to what it hashed before, and B1's fixture
/// stays evidence.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct CompIdentity {
    /// `AEGP_GetItemID` of the comp's item. Unique WITHIN a project, which is
    /// why the key below is not this on its own.
    pub comp_id: i64,
    pub comp_name: String,
    /// `AEGP_GetProjectPath`, empty for a project that has never been saved.
    pub project_path: String,
    pub project_name: String,
    /// False when the project has no file yet, in which case `project_path`
    /// is empty and the key below is weaker. Reported rather than inferred,
    /// because the app says so in words and a guess would be a lie.
    pub saved: bool,
}

impl CompIdentity {
    /// The key a setup is filed under, or `None` when AE could not identify
    /// the comp at all.
    ///
    /// Project path AND comp id, because `AEGP_GetItemID` is unique within a
    /// project and two projects can each have an item 3. Keying on the id
    /// alone would be the same class of bug as keying on a layer name, which
    /// B1 already paid for.
    ///
    /// **Unsaved projects share one prefix.** There is no file to name, so
    /// every unsaved project keys as `<unsaved>#<id>` and two different
    /// unsaved projects CAN collide. That is not swept up: `Setup` carries the
    /// comp's name and `adopt` refuses to apply an unsaved-project setup whose
    /// name does not match, so the collision has to survive both the id and
    /// the name before it can hand somebody else's masses over.
    ///
    /// `None` rather than a fabricated key when `comp_id` is missing: a setup
    /// filed under a guess is a setup that will be handed to the wrong comp,
    /// and the app can say "AE did not identify this comp" instead.
    pub fn key(&self) -> Option<String> {
        if self.comp_id <= 0 {
            return None;
        }
        let project = if self.project_path.is_empty() {
            "<unsaved>"
        } else {
            self.project_path.as_str()
        };
        Some(format!("{project}#{}", self.comp_id))
    }
}

/// The per-layer half of `Params`, filed under one comp.
///
/// ONLY the layer-keyed half. `gravity`, `ppm`, `substeps` and the rest stay
/// global deliberately: the bug C7.1 names is that a value keyed by LAYER ID
/// has nothing above it, so layer 3's mass follows you into another comp's
/// layer 3 -- an id from one comp meaning something in another. A scene-wide
/// gravity carried into the next comp is the same gravity, and moving it here
/// would be a preference change dressed up as a bug fix. Whether the scalars
/// join the setup is C7.2's question, when the sidecar decides what a "setup"
/// is on disk.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Setup {
    /// What the comp was called when this was filed. Shown to the person, and
    /// load-bearing for unsaved projects -- see `CompIdentity::key`.
    pub comp_name: String,
    pub statics: Vec<String>,
    pub layer_params: std::collections::BTreeMap<String, LayerParams>,
}

impl Setup {
    pub fn is_empty(&self) -> bool {
        self.statics.is_empty() && self.layer_params.is_empty()
    }
}

/// What `adopt` did, in the words the window uses.
#[derive(Debug, Clone, Default, Serialize)]
pub struct AdoptReport {
    /// The key now in force, empty when AE could not identify the comp.
    pub key: String,
    /// A setup was already on file for this comp and is now loaded.
    pub restored: bool,
    /// This comp has no setup yet, so the layer-keyed parameters were cleared.
    pub fresh: bool,
    /// AE could not identify the comp, so whatever was loaded stays loaded and
    /// nothing is filed. Said out loud rather than filed under a guess.
    pub unidentified: bool,
    /// A setup existed under this key but was refused: unsaved project, and
    /// the comp name did not match. Names the setup it did not apply.
    pub refused: String,
    /// The setup that was displaced, for the window to say what was kept.
    pub previous: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub paths: Paths,
    pub params: Params,
    pub behaviour: Behaviour,
    /// Every comp's layer-keyed setup, by `CompIdentity::key`.
    pub setups: std::collections::BTreeMap<String, Setup>,
    /// Whose setup is currently loaded into `params`. Empty means none -- the
    /// comp has not been identified, and `params` holds whatever it held.
    pub active_comp: String,
    /// What that comp is called. Held HERE rather than read back out of
    /// `setups`, because a comp being looked at for the first time has no
    /// entry there yet -- and `capture` stamping an empty name onto its first
    /// setup is what makes the unsaved-project name gate fail OPEN. Found by
    /// the test for that gate, which is the only reason it is not shipping.
    pub active_comp_name: String,
    /// The layer-keyed values found in a settings file written before setups
    /// existed. PARKED, NEVER READ.
    ///
    /// Not migrated into the first comp identified, which was the tempting
    /// move. A layer id with no comp above it identifies nothing -- that is
    /// the bug itself -- so adopting those values for whichever comp happened
    /// to be open first would be the bug being committed one last time, by the
    /// fix. Kept rather than deleted because nothing here is worth destroying
    /// to make a point, and the window says where they went.
    pub legacy_setup: Option<Setup>,
    /// The person has read the note about `legacy_setup` and does not want to
    /// be told again. The values stay on file either way -- this dismisses a
    /// sentence, not the data.
    pub legacy_dismissed: bool,
}

impl Settings {
    /// File the layer-keyed half of `params` under whatever comp is active.
    ///
    /// Called before every switch and before every save, because the controls
    /// write straight into `params` -- that is what keeps `solver.rs` and
    /// every existing control unaware that setups exist at all.
    pub fn capture(&mut self) {
        if self.active_comp.is_empty() {
            return;
        }
        let setup = Setup {
            comp_name: self.active_comp_name.clone(),
            statics: self.params.statics.clone(),
            layer_params: self.params.layer_params.clone(),
        };
        if setup.is_empty() {
            // An empty setup is not worth a line in the file, and keeping one
            // would make "this comp has a setup" true for every comp ever
            // looked at -- which is not a fact anybody wants reported.
            self.setups.remove(&self.active_comp);
        } else {
            self.setups.insert(self.active_comp.clone(), setup);
        }
    }

    /// Move a pre-C7.1 file's layer-keyed values out of `params` and into
    /// `legacy_setup`, once. Returns whether there was anything to move.
    ///
    /// Run on load. The condition is "layer-keyed values, no active comp, no
    /// setups and nothing parked yet", which is the exact shape of a settings
    /// file written before this existed -- and it cannot fire twice, because
    /// parking is what makes `legacy_setup` non-None.
    ///
    /// Why park rather than migrate: see `legacy_setup`. Why park rather than
    /// delete: a pinned FLOOR and three masses are somebody's afternoon, and
    /// the fix for an unattributable value is to stop applying it, not to
    /// shred it.
    pub fn park_legacy(&mut self) -> bool {
        if self.legacy_setup.is_some()
            || !self.active_comp.is_empty()
            || !self.setups.is_empty()
        {
            return false;
        }
        let setup = Setup {
            comp_name: String::new(),
            statics: std::mem::take(&mut self.params.statics),
            layer_params: std::mem::take(&mut self.params.layer_params),
        };
        if setup.is_empty() {
            return false;
        }
        self.legacy_setup = Some(setup);
        true
    }

    /// Switch to a comp: file the outgoing setup, load the incoming one.
    pub fn adopt(&mut self, id: &CompIdentity) -> AdoptReport {
        let mut r = AdoptReport::default();

        let Some(key) = id.key() else {
            // Nothing is filed and nothing is cleared. The parameters on
            // screen stay on screen, which is the only honest thing to do
            // with values we cannot attribute to a comp.
            r.unidentified = true;
            return r;
        };
        if key == self.active_comp && id.comp_name == self.active_comp_name {
            // Same comp re-read. Filing and reloading would be a no-op at
            // best and would clobber unsaved edits at worst.
            r.key = key;
            r.restored = true;
            return r;
        }
        /*  The NAME is part of that comparison, not decoration. Two different
            unsaved projects both key as `<unsaved>#7`, so a key-only shortcut
            here returns "same comp, carry on" for what is plainly another
            comp -- and the name gate below never runs. That is the gate
            failing open, which is the one way it could do real harm, and it
            is what the test for it caught. A saved project whose comp was
            merely renamed falls through instead, re-files under the same key
            and reloads its own setup: a longer route to the same place. */
        self.capture();
        r.previous = self.active_comp.clone();
        r.key = key.clone();

        match self.setups.get(&key) {
            Some(s) if !id.saved && !s.comp_name.is_empty()
                        && s.comp_name != id.comp_name => {
                // Unsaved project: the key is `<unsaved>#<id>` and two
                // different unsaved projects can reach it. The name is the
                // second gate, and a setup that fails it is not applied.
                r.refused = s.comp_name.clone();
                self.params.statics.clear();
                self.params.layer_params.clear();
                self.setups.remove(&key);
                r.fresh = true;
            }
            Some(s) => {
                self.params.statics = s.statics.clone();
                self.params.layer_params = s.layer_params.clone();
                r.restored = true;
            }
            None => {
                // A comp with no setup starts EMPTY, not from the last comp's
                // values. Inheriting them is exactly the bug.
                self.params.statics.clear();
                self.params.layer_params.clear();
                r.fresh = true;
            }
        }
        self.active_comp = key.clone();
        // The name is refreshed on every adopt, so renaming a comp in a saved
        // project keeps its setup -- the id is what identifies it there.
        self.active_comp_name = id.comp_name.clone();
        if let Some(s) = self.setups.get_mut(&key) {
            s.comp_name = id.comp_name.clone();
        }
        r
    }
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
    let mut s: Settings = serde_json::from_str(&text).unwrap_or_else(|e| {
        eprintln!("{}: ignoring unreadable settings ({e})", path.display());
        Settings::default()
    });
    /*  C7.1. A file written before setups existed carries layer-keyed values
        with no comp above them, which is the bug itself in stored form. They
        are moved aside HERE, on the way in, so that nothing downstream ever
        sees them as live parameters -- and so the move happens once, on the
        first launch after the upgrade, rather than being re-decided on every
        read. See `park_legacy`. */
    if s.park_legacy() {
        eprintln!(
            "{}: layer-keyed parameters from before setups existed have been \
             parked under `legacy_setup` -- they are kept, not applied",
            path.display()
        );
    }
    s
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

#[cfg(test)]
mod tests {
    use super::*;

    fn id(project: &str, comp_id: i64, name: &str) -> CompIdentity {
        CompIdentity {
            comp_id,
            comp_name: name.into(),
            project_path: project.into(),
            project_name: String::new(),
            saved: !project.is_empty(),
        }
    }

    /// Give the active comp a pinned layer and a mass on layer 3, the way the
    /// controls do -- straight into `params`.
    fn author(s: &mut Settings, statics: &[&str], mass_on: &str, mass: f64) {
        s.params.statics = statics.iter().map(|x| x.to_string()).collect();
        s.params.layer_params.insert(
            mass_on.into(),
            LayerParams { mass: Some(mass), ..Default::default() },
        );
    }

    /// THE BUG C7.1 NAMES, as a test. Layer 3's mass must not follow you into
    /// another comp's layer 3.
    #[test]
    fn a_layer_id_does_not_mean_anything_in_the_next_comp() {
        let mut s = Settings::default();
        s.adopt(&id("C:\\p.aep", 7, "Comp 1"));
        author(&mut s, &["4"], "3", 12.0);

        let r = s.adopt(&id("C:\\p.aep", 9, "Comp 2"));

        assert!(r.fresh, "a comp with no setup must start fresh");
        assert!(
            s.params.layer_params.is_empty() && s.params.statics.is_empty(),
            "layer-keyed values leaked into another comp: statics {:?}, \
             layer_params {:?}",
            s.params.statics,
            s.params.layer_params
        );
    }

    /// The broken control. If switching back did NOT restore, the test above
    /// would pass on a version that simply throws every setup away -- which is
    /// not a fix, it is amnesia. Both halves have to hold.
    #[test]
    fn switching_back_restores_that_comps_own_setup() {
        let mut s = Settings::default();
        s.adopt(&id("C:\\p.aep", 7, "Comp 1"));
        author(&mut s, &["4"], "3", 12.0);
        s.adopt(&id("C:\\p.aep", 9, "Comp 2"));
        author(&mut s, &["1"], "2", 99.0);

        let r = s.adopt(&id("C:\\p.aep", 7, "Comp 1"));

        assert!(r.restored);
        assert_eq!(s.params.statics, vec!["4".to_string()]);
        assert_eq!(s.params.layer_params["3"].mass, Some(12.0));
        assert!(!s.params.layer_params.contains_key("2"), "Comp 2 bled back");
    }

    /// `AEGP_GetItemID` is unique WITHIN a project. Item 7 of one project and
    /// item 7 of another are different comps, and keying on the id alone would
    /// be the same class of mistake as keying on a layer name.
    #[test]
    fn the_same_item_id_in_another_project_is_another_comp() {
        let mut s = Settings::default();
        s.adopt(&id("C:\\a.aep", 7, "Comp 1"));
        author(&mut s, &["4"], "3", 12.0);

        let r = s.adopt(&id("C:\\b.aep", 7, "Comp 1"));

        assert!(r.fresh, "same id, different project: {r:?}");
        assert!(s.params.layer_params.is_empty());
    }

    /// An unsaved project has no path, so every unsaved project keys under one
    /// prefix and CAN collide. The name is the second gate: a setup that fails
    /// it is refused and said out loud, not handed over.
    #[test]
    fn an_unsaved_projects_setup_is_refused_when_the_name_disagrees() {
        let mut s = Settings::default();
        s.adopt(&id("", 7, "Bouncing balls"));
        author(&mut s, &["4"], "3", 12.0);

        // A different unsaved project, whose comp happens to be item 7 too.
        let r = s.adopt(&id("", 7, "Title card"));

        assert_eq!(r.refused, "Bouncing balls");
        assert!(r.fresh);
        assert!(s.params.layer_params.is_empty());
    }

    /// In a SAVED project the id is what identifies the comp, so renaming one
    /// must keep its setup. The asymmetry with the test above is deliberate:
    /// there the name is carrying weight the path cannot, here it is not.
    #[test]
    fn renaming_a_comp_in_a_saved_project_keeps_its_setup() {
        let mut s = Settings::default();
        s.adopt(&id("C:\\p.aep", 7, "Comp 1"));
        author(&mut s, &["4"], "3", 12.0);
        s.adopt(&id("C:\\p.aep", 9, "Comp 2"));

        let r = s.adopt(&id("C:\\p.aep", 7, "Balls"));

        assert!(r.restored, "a rename must not lose the setup: {r:?}");
        assert_eq!(s.params.layer_params["3"].mass, Some(12.0));
        assert_eq!(s.setups["C:\\p.aep#7"].comp_name, "Balls",
                   "the stored name must follow the rename");
    }

    /// AE could not say which comp this is. Filing under a guess would hand
    /// these values to the wrong comp later, so nothing is filed and nothing
    /// is cleared -- what is on screen stays on screen, and the window says so.
    #[test]
    fn an_unidentified_comp_files_nothing_and_clears_nothing() {
        let mut s = Settings::default();
        s.adopt(&id("C:\\p.aep", 7, "Comp 1"));
        author(&mut s, &["4"], "3", 12.0);

        let r = s.adopt(&id("C:\\p.aep", 0, "Comp 1"));

        assert!(r.unidentified);
        assert_eq!(s.active_comp, "C:\\p.aep#7", "the active comp must not move");
        assert_eq!(s.params.layer_params["3"].mass, Some(12.0));
    }

    /// Re-reading the SAME comp must not clobber edits made since the read.
    /// `adopt` runs on every read, and a reload here would silently undo
    /// whatever was typed between the two.
    #[test]
    fn re_reading_the_same_comp_does_not_reload_over_unsaved_edits() {
        let mut s = Settings::default();
        s.adopt(&id("C:\\p.aep", 7, "Comp 1"));
        author(&mut s, &["4"], "3", 12.0);
        s.capture();
        s.params.layer_params.get_mut("3").unwrap().mass = Some(40.0);

        s.adopt(&id("C:\\p.aep", 7, "Comp 1"));

        assert_eq!(s.params.layer_params["3"].mass, Some(40.0));
    }

    /// An empty setup is not filed. Otherwise "this comp has a setup" becomes
    /// true for every comp ever looked at, which is not a fact worth reporting.
    #[test]
    fn a_comp_with_nothing_set_leaves_no_entry_behind() {
        let mut s = Settings::default();
        s.adopt(&id("C:\\p.aep", 7, "Comp 1"));
        s.adopt(&id("C:\\p.aep", 9, "Comp 2"));
        assert!(s.setups.is_empty(), "{:?}", s.setups);
    }

    /// The scene-wide scalars are NOT per comp, and that is a decision rather
    /// than an omission -- see `Setup`. If this ever starts failing, the
    /// decision changed and the comment above it has to change with it.
    #[test]
    fn scene_wide_scalars_are_deliberately_not_part_of_a_setup() {
        let mut s = Settings::default();
        s.adopt(&id("C:\\p.aep", 7, "Comp 1"));
        s.params.gravity = 3.7;
        s.params.ppm = 200.0;

        s.adopt(&id("C:\\p.aep", 9, "Comp 2"));

        assert_eq!(s.params.gravity, 3.7);
        assert_eq!(s.params.ppm, 200.0);
    }

    /// A settings file written before setups existed has layer-keyed values
    /// with no comp above them. They must not be adopted by whichever comp is
    /// opened first -- that is the bug being committed one last time, by the
    /// fix -- and they must not be destroyed either.
    #[test]
    fn legacy_values_are_parked_not_adopted() {
        let old = r#"{"params":{"statics":["4"],
                      "layer_params":{"3":{"mass":12.0}}}}"#;
        let mut s: Settings = serde_json::from_str(old).unwrap();
        assert_eq!(s.params.layer_params["3"].mass, Some(12.0),
                   "the old file must still load");

        let parked = Settings::park_legacy(&mut s);

        assert!(parked, "there was something to park");
        assert!(s.params.statics.is_empty());
        assert!(s.params.layer_params.is_empty());
        let kept = s.legacy_setup.as_ref().expect("nothing was destroyed");
        assert_eq!(kept.statics, vec!["4".to_string()]);
        assert_eq!(kept.layer_params["3"].mass, Some(12.0));

        // Idempotent: a second launch must not park an empty setup over the
        // real one and lose it.
        assert!(!Settings::park_legacy(&mut s));
        assert_eq!(s.legacy_setup.as_ref().unwrap().layer_params["3"].mass,
                   Some(12.0));
    }
}
