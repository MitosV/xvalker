//! The test vectors: what the reference decoder makes of a BCPack chunk, and
//! what ours makes of it.
//!
//! The vectors are in `tests/data/`, made by `tools/bcpack/make_fixtures.py`
//! with the reference project's encoder. They exercise the block rebuilders
//! of BC1, BC3, BC4 and BC5.

use std::fs;

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
    if hr != 0 {
        None
    } else {
        Some(out)
    }
}

#[test]
fn the_vectors_decode_as_the_reference_made_them() {
    for (name, mode, size) in [
        ("bc1", 1u32, 24usize),
        ("bc3", 3, 16),
        ("bc4", 4, 16),
        ("bc5", 5, 32),
    ] {
        let chunk = fs::read(format!("tests/data/{name}.bcpack"))
            .unwrap_or_else(|e| panic!("{name}.bcpack: {e}"));
        let expected = fs::read(format!("tests/data/{name}.expected"))
            .unwrap_or_else(|e| panic!("{name}.expected: {e}"));
        assert_eq!(expected.len(), size, "{name}");
        let out = decode(mode, &chunk, size)
            .unwrap_or_else(|| panic!("{name} did not decode at all"));
        assert_eq!(out, expected, "{name}");
    }
}

#[test]
fn a_chunk_that_is_not_one_is_refused() {
    let chunk = fs::read("tests/data/bc1.bcpack").unwrap();
    // a destination that is not a whole number of blocks
    let mut out = vec![0u8; 12];
    // SAFETY: the buffers are readable and writable for the sizes given.
    let hr = unsafe {
        bcpack::xv_bcpack_decode(1, chunk.as_ptr(), chunk.len(), out.as_mut_ptr(), out.len())
    };
    assert_eq!(hr, 0x8924_000fu32 as i32, "E_DSTORAGE_INVALID_DESTINATION_SIZE");
    // a mode without a public block mapping
    // SAFETY: as above.
    let hr = unsafe {
        bcpack::xv_bcpack_decode(7, chunk.as_ptr(), chunk.len(), out.as_mut_ptr(), out.len())
    };
    assert_eq!(hr, 0x8924_0030u32 as i32, "E_DSTORAGE_DECOMPRESSION_ERROR");
}
