//! The small `crate::request` surface the vendored `xcrd_tests.rs` exercises.
//!
//! It mirrors the emulator's request execution order: DEFLATE (when the zlib
//! bit is set), then BCPack. The production path is `dstorage.cpp`, not this
//! test-only helper.

use crate::abi::*;
use crate::bcpack;

pub enum Source<'a> {
    File(&'a [u8]),
    Memory(&'a [u8]),
}

#[derive(Debug, PartialEq, Eq)]
pub struct Outcome {
    pub written: usize,
}

fn source_bytes(src: Source<'_>, source_size: u32) -> Result<Vec<u8>, i32> {
    let bytes = match src {
        Source::File(b) | Source::Memory(b) => b,
    };
    let size = source_size as usize;
    if size > bytes.len() {
        return Err(E_DSTORAGE_ACCESS_VIOLATION);
    }
    Ok(bytes[..size].to_vec())
}

/// `compression` is the low byte of the console's request options (zlib bit and
/// BCPack mode). `intermediate` is the decompressed size of the DEFLATE stage,
/// or 0 when the destination size is the stage size.
pub fn execute_with_intermediate(
    compression: u8,
    intermediate: u32,
    src: Source<'_>,
    source_size: u32,
    dst: &mut [u8],
) -> Result<Outcome, i32> {
    let mode = ((compression as u64 >> OPT_BCPACK_SHIFT) & OPT_BCPACK_MASK) as u8;
    let packed = source_bytes(src, source_size)?;
    let chunk = if compression as u64 & OPT_ZLIB != 0 {
        let want = if intermediate != 0 {
            intermediate as usize
        } else {
            dst.len()
        };
        let mut mid = miniz_oxide::inflate::decompress_to_vec_zlib_with_limit(&packed, want)
            .map_err(|_| E_DSTORAGE_ZLIB_BAD_DATA)?;
        if mid.len() != want {
            return Err(E_DSTORAGE_ZLIB_BAD_DATA);
        }
        if mid.len() < dst.len() {
            mid.resize(dst.len(), 0);
        }
        mid
    } else {
        packed
    };
    if mode == 0 {
        if chunk.len() < dst.len() {
            return Err(E_DSTORAGE_INVALID_DESTINATION_SIZE);
        }
        dst.copy_from_slice(&chunk[..dst.len()]);
        return Ok(Outcome {
            written: dst.len(),
        });
    }
    let written = bcpack::decode(mode, &chunk, dst)?;
    Ok(Outcome { written })
}
