//! Wall K, on the side of the loop that can actually see both documents.
//!
//! WHY THIS EXISTS AT ALL
//! ----------------------
//! B3 puts a `source` block on every bake -- the scene file's sha256, the
//! comp's identity, the layer list, the settings -- and `b2_apply_bake.jsx`
//! checks it before writing a single keyframe. But its own comment says what
//! it is actually able to check: "Comp identity is the half AE can check."
//!
//! The bake's `source` block describes THE SCENE. The apply script can only
//! see THE COMP. The only fields those two documents share are names and
//! dimensions, so that is the whole of the guard:
//!
//! ```text
//! comp name  |  comp dimensions  |  layer id in range  |  layer name at id
//! ```
//!
//! Every geometric field -- position, rotation, scale, anchor, paths -- lives
//! in the scene and is invisible from inside AE. Measured 2026-09-10: nudge a
//! layer's Position after simulating and the bake applies anyway, overwriting
//! the nudge from keyframe 0. Well-formed, validating, matching on every name,
//! and wrong. That is Wall K's exact failure, and the guard does not cover it.
//!
//! `b3_loop.py --check` DOES compare the hash, at b3_loop.py:97 -- but against
//! a scene file on disk. AE has no scene file. The hash is not ignored there;
//! it is unreachable.
//!
//! WHAT CLOSES IT
//! --------------
//! The roadmap's C3 in one sentence: "re-read and compare, never trust the
//! source block." The shell can do what the .jsx cannot, because it already
//! owns `read_scene` -- so it asks AE for the comp AGAIN, hashes what comes
//! back, and compares that to the hash the bake was made under.
//!
//! This is sound only because the scene document is DETERMINISTIC: it carries
//! no timestamp and no ordering that depends on when it was read, so the same
//! comp hashes the same twice. (`made_at` lives on the BAKE, not the scene.)
//! If a nondeterministic field is ever added to `ae-physics-scene`, this guard
//! turns into a permanent false alarm -- and a guard that always fires is a
//! guard nobody reads.
//!
//! WHY IT REFUSES RATHER THAN WARNS
//! --------------------------------
//! Deliberately not symmetrical with the solver's escape guard, which exits 3
//! and says "no". A stale bake is not always wrong: re-reading after moving a
//! layer you did not simulate is a legitimate thing to do, and the honest
//! answer is "this bake was computed from different geometry", not "you may
//! not". So this reports, names what it can, and leaves the decision with the
//! person -- but it never reports OK on a hash it could not verify.

use serde::Serialize;
use sha2::{Digest, Sha256};

/// What the bake says it was made from, and what AE says is there now.
#[derive(Debug, Serialize)]
pub struct Verdict {
    /// The hashes match: the comp is byte-for-byte the scene this bake used.
    pub fresh: bool,
    /// Named differences, in the language the apply script would use. Empty
    /// with `fresh == false` means the change is geometric -- real, and with
    /// nothing in the comp's identity to point at.
    pub reasons: Vec<String>,
    pub bake_sha256: String,
    pub live_sha256: String,
    /// When the bake was computed, straight from the source block.
    pub made_at: String,
    /// The comp the bake was made from, for the window to show beside the
    /// comp that is open now.
    pub bake_comp: String,
    pub live_comp: String,
}

pub fn sha256_hex(bytes: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(bytes);
    format!("{:x}", h.finalize())
}

fn s(v: Option<&serde_json::Value>) -> String {
    v.and_then(|x| x.as_str()).unwrap_or("").to_string()
}

/// Compare a bake against a scene document just read from AE.
///
/// `live` is the raw bytes off the pipe, NOT a re-serialised document: the
/// hash is over the file B3 would have been handed, and a round trip through
/// a JSON library re-orders keys and re-formats floats. C0.1's whole pass
/// criterion was byte-identity, and this is the same property being used.
pub fn compare(bake: &serde_json::Value, live: &[u8]) -> Result<Verdict, String> {
    let src = bake
        .get("source")
        .ok_or("this bake has no `source` block, so there is nothing to check \
                it against -- it predates B3's staleness guard and cannot be \
                verified. Re-run the solver.")?;

    let bake_sha = s(src.get("scene_sha256"));
    if bake_sha.is_empty() {
        return Err("this bake's `source` block has no scene_sha256".into());
    }
    let live_sha = sha256_hex(live);

    let live_doc: serde_json::Value = serde_json::from_slice(live)
        .map_err(|e| format!("what AE returned is not JSON ({e})"))?;

    let bc = src.get("comp");
    let lc = live_doc.get("comp");
    let bake_comp = s(bc.and_then(|c| c.get("name")));
    let live_comp = s(lc.and_then(|c| c.get("name")));

    // The same four checks the .jsx makes, so that a difference it WOULD have
    // caught is reported in the same words. The hash is what catches the rest.
    let mut reasons = Vec::new();
    if bake_comp != live_comp {
        reasons.push(format!(
            "bake was made from comp '{bake_comp}', this is '{live_comp}'"
        ));
    }
    let dim = |c: Option<&serde_json::Value>, k: &str| {
        c.and_then(|c| c.get(k)).and_then(|v| v.as_i64()).unwrap_or(-1)
    };
    if dim(bc, "width") != dim(lc, "width") || dim(bc, "height") != dim(lc, "height") {
        reasons.push(format!(
            "bake was made at {}x{}, this comp is {}x{}",
            dim(bc, "width"),
            dim(bc, "height"),
            dim(lc, "width"),
            dim(lc, "height")
        ));
    }

    let empty = vec![];
    let bl = src.get("layers").and_then(|v| v.as_array()).unwrap_or(&empty);
    let ll = live_doc.get("layers").and_then(|v| v.as_array()).unwrap_or(&empty);
    for b in bl {
        // Joined on `id`, never on name: B1 found AE allows duplicate layer
        // names, and Phase A silently dropped a layer for keying on them.
        let id = b.get("id").and_then(|v| v.as_i64());
        match ll.iter().find(|l| l.get("id").and_then(|v| v.as_i64()) == id) {
            None => reasons.push(format!(
                "id {} ({}) is not in this comp any more",
                id.unwrap_or(-1),
                s(b.get("name"))
            )),
            Some(l) => {
                let (bn, ln) = (s(b.get("name")), s(l.get("name")));
                if bn != ln {
                    reasons.push(format!("id {}: bake says '{bn}', comp has '{ln}'",
                                         id.unwrap_or(-1)));
                }
            }
        }
    }

    Ok(Verdict {
        fresh: bake_sha == live_sha,
        reasons,
        bake_sha256: bake_sha,
        live_sha256: live_sha,
        made_at: s(src.get("made_at")),
        bake_comp,
        live_comp,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    /// A scene, and the bake that would have been made from it.
    fn pair(comp: &str, pos: [i64; 2]) -> (Vec<u8>, serde_json::Value) {
        let scene = serde_json::to_vec(&json!({
            "schema": "ae-physics-scene/2",
            "comp": {"name": comp, "width": 1920, "height": 1080, "fps": 24},
            "layers": [
                {"id": 1, "name": "Shape Layer 1", "position": pos},
                {"id": 3, "name": "Shape Layer 1", "position": [992, 110]},
            ],
        }))
        .unwrap();
        let bake = json!({
            "schema": "ae-physics-bake/3",
            "source": {
                "scene_sha256": sha256_hex(&scene),
                "made_at": "2026-09-10T00:13:49",
                "comp": {"name": comp, "width": 1920, "height": 1080},
                "layers": [
                    {"id": 1, "name": "Shape Layer 1"},
                    {"id": 3, "name": "Shape Layer 1"},
                ],
            },
        });
        (scene, bake)
    }

    #[test]
    fn unedited_comp_is_fresh() {
        let (scene, bake) = pair("Comp 1", [1064, 302]);
        let v = compare(&bake, &scene).unwrap();
        assert!(v.fresh, "an untouched comp must verify");
        assert!(v.reasons.is_empty());
    }

    /// THE ONE THAT MATTERS. This is the edit measured in AE on 2026-09-10:
    /// a layer's Position nudged after the bake. Every field the apply script
    /// can see is unchanged -- comp name, dimensions, layer ids, layer names --
    /// so `reasons` is EMPTY and the hash is the only thing that catches it.
    #[test]
    fn a_nudged_position_is_caught_and_only_by_the_hash() {
        let (_, bake) = pair("Comp 1", [1064, 302]);
        let (nudged, _) = pair("Comp 1", [1070, 302]);
        let v = compare(&bake, &nudged).unwrap();
        assert!(!v.fresh, "a moved layer must not verify");
        assert!(
            v.reasons.is_empty(),
            "the identity checks must NOT fire here -- if they do, this test \
             is passing for the wrong reason and no longer proves the hash is \
             load-bearing: {:?}",
            v.reasons
        );
    }

    /// The broken control: the guard must still be able to name what the .jsx
    /// would have named. A guard that only ever says "something changed" is
    /// not obviously better than the one it replaces.
    #[test]
    fn a_renamed_comp_is_named_not_just_hashed() {
        let (_, bake) = pair("Comp 1", [1064, 302]);
        let (other, _) = pair("Comp 2", [1064, 302]);
        let v = compare(&bake, &other).unwrap();
        assert!(!v.fresh);
        assert!(v.reasons.iter().any(|r| r.contains("'Comp 1'")
            && r.contains("'Comp 2'")));
    }

    /// B1: AE allows duplicate layer names, and Phase A silently dropped a
    /// layer for keying on them. Deleting id 3 must be reported against ITS
    /// id, not swallowed by id 1 happening to share its name.
    #[test]
    fn a_deleted_layer_is_joined_on_id_not_name() {
        let (_, bake) = pair("Comp 1", [1064, 302]);
        let short = serde_json::to_vec(&json!({
            "schema": "ae-physics-scene/2",
            "comp": {"name": "Comp 1", "width": 1920, "height": 1080, "fps": 24},
            "layers": [{"id": 1, "name": "Shape Layer 1", "position": [1064, 302]}],
        }))
        .unwrap();
        let v = compare(&bake, &short).unwrap();
        assert!(!v.fresh);
        assert!(
            v.reasons.iter().any(|r| r.starts_with("id 3")),
            "expected id 3 to be named, got {:?}",
            v.reasons
        );
    }

    /// A bake with no `source` block predates the guard. It must refuse to
    /// answer rather than answer "fresh" -- never report OK on a hash it could
    /// not verify.
    #[test]
    fn a_bake_without_a_source_block_is_an_error_not_a_pass() {
        let (scene, _) = pair("Comp 1", [1064, 302]);
        let err = compare(&json!({"schema": "ae-physics-bake/3"}), &scene).unwrap_err();
        assert!(err.contains("no `source` block"), "{err}");
    }
}
