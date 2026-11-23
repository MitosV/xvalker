//! The game's own BCPack chunks: what the reference decoder makes of them,
//! and what ours makes of them.
//!
//! The chunks are in `~/.cache/xvalker/bcpack-samples`, written by
//! `tools/bcpack/extract_samples.py` with the reference project's tools.
//! Without them this test does nothing: the samples are the game's data and
//! are not in the repository.

use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

fn samples() -> Option<PathBuf> {
    if let Ok(folder) = std::env::var("XV_BCPACK_SAMPLES") {
        return Some(PathBuf::from(folder));
    }
    let home = std::env::var("HOME").ok()?;
    let folder = PathBuf::from(home).join(".cache/xvalker/bcpack-samples");
    folder.exists().then_some(folder)
}

fn decode(mode: u32, chunk: &[u8], size: usize) -> Option<Vec<u8>> {
    let mut out = vec![0u8; size];
    // SAFETY: the chunk and the destination are readable and writable for the
    // sizes given.
    let hr = unsafe {
        bcpack::xv_bcpack_decode(
            mode,
            chunk.as_ptr(),
            chunk.len(),
            out.as_mut_ptr(),
            out.len(),
        )
    };
    (hr == 0).then_some(out)
}

#[test]
fn the_game_chunks_decode_as_the_reference_made_them() {
    let Some(folder) = samples() else {
        eprintln!("no BCPack samples: skipped");
        return;
    };
    let manifest = folder.join("manifest.txt");
    let Ok(manifest) = fs::read_to_string(&manifest) else {
        eprintln!("no {}: skipped", manifest.display());
        return;
    };
    // per mode: the chunks the reference decoded and we decoded the same, the
    // ones we decoded differently, the ones we do not decode, the ones we
    // decode where the reference does not, and the ones the reference decodes
    // and we do not
    let mut counts: BTreeMap<u32, [usize; 5]> = BTreeMap::new();
    for line in manifest.lines() {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.len() < 5 {
            continue;
        }
        let (base, mode, size, result) = (
            fields[0],
            fields[1].parse::<u32>().unwrap(),
            fields[3].parse::<usize>().unwrap(),
            fields[4],
        );
        let Ok(chunk) = fs::read(folder.join(format!("{base}.bcpack"))) else {
            continue;
        };
        let ours = decode(mode, &chunk, size);
        let theirs = if result == "decoded" {
            fs::read(folder.join(format!("{base}.expected"))).ok()
        } else {
            None
        };
        let entry = counts.entry(mode).or_insert([0; 5]);
        match (&ours, &theirs) {
            (Some(ours), Some(theirs)) => {
                if ours == theirs {
                    entry[0] += 1;
                } else {
                    entry[1] += 1;
                }
            }
            (None, None) => entry[2] += 1,
            (Some(_), None) => entry[3] += 1,
            (None, Some(_)) => entry[4] += 1,
        }
    }
    for (mode, counts) in &counts {
        eprintln!(
            "BCPack mode {mode}: {} equal, {} different, {} unsupported, {} only ours, {} only theirs",
            counts[0], counts[1], counts[2], counts[3], counts[4]
        );
    }
    for (mode, counts) in &counts {
        assert_eq!(counts[1], 0, "mode {mode}: the blocks differ from the reference");
        assert_eq!(counts[4], 0, "mode {mode}: the reference decoded a chunk we did not");
    }
    // BC6H and BC7 have no confirmed public block mapping: none of theirs decodes
    for mode in [6u32, 7] {
        if let Some(counts) = counts.get(&mode) {
            assert_eq!(counts[0], 0, "mode {mode} should not decode");
            assert_eq!(counts[3], 0, "mode {mode} should not decode");
        }
    }
}
