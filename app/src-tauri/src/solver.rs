//! Running the solver, which is `b3_loop.py` in a subprocess.
//!
//! WHY A SUBPROCESS AND NOT A PORT
//! -------------------------------
//! The plan is explicit that the first version ships Python rather than porting
//! to Rapier, and the reason is not sentiment: switching solvers discards A1's
//! falsified integrator finding and the measured ppm 10-1000 band, both of
//! which are engine-specific, and the geometry pipeline is ~1,200 lines of
//! *verified* code that would need porting AND re-verifying.
//!
//! WHY THE COMMAND LINE AND NOT AN IMPORT
//! --------------------------------------
//! B3's whole claim is that the middle of the loop is ONE COMMAND, and
//! `b3_checks.py` deliberately runs that command in a subprocess rather than
//! calling `run()` in-process -- "testing the functions would not test the
//! tool". The shell is a caller of the same tool, and driving it any other way
//! would make the GUI a second implementation of the loop with its own bugs.
//!
//! So this file is a translation of a command line and nothing else. Every
//! argument below appears in `b3_loop.py --help`, and the flags this app does
//! not expose (`--check`, `--no-preview`) are absent rather than defaulted.
//!
//! EXIT CODES ARE THE INTERFACE
//! ----------------------------
//! B3 exits 3 with nothing written when a layer escapes the comp -- the guard
//! B2 found by accident, after a bake integrated to 839,091 px and was written
//! into the project as real keyframes. That is a REFUSAL, not a crash, and it
//! reaches the user as its own outcome rather than as "the solver failed".

use crate::settings::{Params, Paths};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::Command;

pub const SOLVER: &str = "b3_loop.py";

/// B3's exit code for "a layer left the comp and I will not write this".
const EXIT_ESCAPED: i32 = 3;

#[derive(Debug, Serialize)]
pub struct SolveResult {
    pub ok: bool,
    /// True only for the escape guard: the run is complete and the answer is
    /// "no". The UI says so in different words from a failure.
    pub refused: bool,
    pub bake_path: String,
    pub preview_path: String,
    /// The viewport's geometry. Empty when the run refused or failed, for the
    /// same reason `bake_path` is: there is nothing to look at.
    pub render_path: String,
    pub stdout: String,
    pub stderr: String,
    pub exit_code: Option<i32>,
    pub command: String,
}

fn quote(s: &str) -> String {
    if s.contains(' ') {
        format!("\"{s}\"")
    } else {
        s.to_string()
    }
}

pub fn solve(
    paths: &Paths,
    params: &Params,
    scene_path: &Path,
    out_dir: &Path,
) -> Result<SolveResult, String> {
    let script = paths.script(SOLVER);
    if !script.exists() {
        return Err(format!(
            "{} is not in the prototype folder.\n\n{} does not look like a \
             python-proto/physics_sim checkout -- that is the folder holding \
             {SOLVER} and b1_read_shapes.jsx.",
            script.display(),
            if paths.proto_dir.is_empty() {
                "(no folder is set)"
            } else {
                &paths.proto_dir
            }
        ));
    }

    let bake: PathBuf = out_dir.join("bake.json");
    let preview: PathBuf = out_dir.join("preview.png");
    /*  C2. The viewport draws polygons, and turning a comp into polygons is
        the ~1,200 lines of geometry A3 and A4 verified -- so B3 emits them and
        the app never interprets a bezier. The transform on top of them is the
        three lines the viewport cannot avoid owning. */
    let render: PathBuf = out_dir.join("render.json");

    let mut args: Vec<String> = vec![
        script.display().to_string(),
        scene_path.display().to_string(),
        "--out".into(),
        bake.display().to_string(),
        "--preview".into(),
        preview.display().to_string(),
        "--gravity".into(),
        params.gravity.to_string(),
        "--ppm".into(),
        params.ppm.to_string(),
        "--substeps".into(),
        params.substeps.to_string(),
        "--friction".into(),
        params.friction.to_string(),
        "--elasticity".into(),
        params.elasticity.to_string(),
        "--render-model".into(),
        render.display().to_string(),
    ];
    // Absent, not defaulted: with no --frames, B3 uses the comp's duration,
    // and there is no number this app could pass that means the same thing.
    if let Some(f) = params.frames {
        args.push("--frames".into());
        args.push(f.to_string());
    }
    for s in &params.statics {
        args.push("--static".into());
        args.push(s.clone());
    }
    if params.no_walls {
        args.push("--no-walls".into());
    }
    if params.allow_escapes {
        args.push("--allow-escapes".into());
    }

    let shown = format!(
        "{} {}",
        quote(&paths.python),
        args.iter().map(|a| quote(a)).collect::<Vec<_>>().join(" ")
    );

    let out = Command::new(&paths.python)
        .args(&args)
        // The prototype imports its neighbours by bare name (`import sim`,
        // `import scene_io`), so it has to run from its own folder.
        .current_dir(&paths.proto_dir)
        .output()
        .map_err(|e| {
            format!(
                "could not start {:?}: {e}\n\nSet the interpreter in Settings. \
                 A bare name is looked up on PATH; an absolute path pins a venv.",
                paths.python
            )
        })?;

    let code = out.status.code();
    Ok(SolveResult {
        ok: out.status.success(),
        refused: code == Some(EXIT_ESCAPED),
        bake_path: if out.status.success() {
            bake.display().to_string()
        } else {
            String::new()
        },
        preview_path: if out.status.success() && preview.exists() {
            preview.display().to_string()
        } else {
            String::new()
        },
        render_path: if out.status.success() && render.exists() {
            render.display().to_string()
        } else {
            String::new()
        },
        stdout: String::from_utf8_lossy(&out.stdout).into_owned(),
        stderr: String::from_utf8_lossy(&out.stderr).into_owned(),
        exit_code: code,
        command: shown,
    })
}

/// Does this folder look like the prototype, and does that interpreter run?
///
/// Checked on demand from Settings rather than at startup: the app has to open
/// with nothing configured, and an error on launch that the user cannot act on
/// before the window exists is not a diagnosis.
pub fn probe(paths: &Paths) -> Result<String, String> {
    let script = paths.script(SOLVER);
    if !script.exists() {
        return Err(format!("{} does not exist", script.display()));
    }
    let out = Command::new(&paths.python)
        .arg("-c")
        .arg("import sys, pymunk; print(sys.version.split()[0], pymunk.version)")
        .current_dir(&paths.proto_dir)
        .output()
        .map_err(|e| format!("could not run {:?}: {e}", paths.python))?;
    if !out.status.success() {
        // pymunk missing is the overwhelmingly likely cause, and it is
        // actionable, so the stderr goes through rather than a summary.
        return Err(format!(
            "the interpreter ran but the prototype's dependencies did not \
             import:\n\n{}",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    let v = String::from_utf8_lossy(&out.stdout);
    let mut it = v.split_whitespace();
    Ok(format!(
        "Python {} with pymunk {} -- {} found",
        it.next().unwrap_or("?"),
        it.next().unwrap_or("?"),
        SOLVER
    ))
}
