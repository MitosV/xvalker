//! `bcpack.dll`: xvalker's BCPack decoder, behind the C ABI that
//! `src/runtime/modules/dstorage.cpp` loads lazily.
//!
//! The BC1 decoder and the rANS lane decoder are the vendored, byte-identical
//! `xbox-emulator` module in `third_party/bcpack/`. Modes without a confirmed
//! public block mapping (BC2, BC3, BC4, BC5, BC6H, BC7) fail with
//! `E_DSTORAGE_DECOMPRESSION_ERROR` and a log line.

extern crate self as compat_common;

pub mod abi;

#[cfg(test)]
pub mod request;

mod compat_common_shim;

// The vendored module reaches for `compat_common::online::util::sha256`.
pub use compat_common_shim::online;

pub use abi::{
    E_DSTORAGE_DECOMPRESSION_ERROR, E_DSTORAGE_INVALID_DESTINATION_SIZE, S_OK,
};

/// One diagnostic line on stderr, in the runtime's usual `[prefix] ...` shape.
pub fn record_raw(category: &str, what: &str, message: &str) {
    use std::io::Write;
    let line = format!("[{category}] {what}: {message}\n");
    let _ = std::io::stderr().write_all(line.as_bytes());
}

#[path = "../../../third_party/bcpack/mod.rs"]
pub mod bcpack;

fn fail(message: String) -> i32 {
    record_raw("bcpack", "BCPack", &message);
    E_DSTORAGE_DECOMPRESSION_ERROR
}

/// Decode one BCPack chunk.
///
/// * `mode`: `DSTORAGE_BCPACK_MODE` (1 = BC1).
/// * `src`, `src_size`: the packed chunk.
/// * `dst`, `dst_size`: the destination; `dst_size` is the chunk's decoded
///   byte count and must be a whole number of blocks for the mode.
///
/// Returns `S_OK` on success, `E_DSTORAGE_INVALID_DESTINATION_SIZE` for a
/// destination that is not a whole number of blocks, or
/// `E_DSTORAGE_DECOMPRESSION_ERROR` for an unsupported mode or a chunk that
/// does not decode under the confirmed tables. Nothing is written on error.
///
/// # Safety
/// `src` must point to `src_size` readable bytes and `dst` to `dst_size`
/// writable bytes.
#[no_mangle]
pub unsafe extern "C" fn xv_bcpack_decode(
    mode: u32,
    src: *const u8,
    src_size: usize,
    dst: *mut u8,
    dst_size: usize,
) -> i32 {
    if src.is_null() || dst.is_null() || src_size == 0 || dst_size == 0 {
        return fail(format!(
            "invalid request (mode {mode}, source {src_size} bytes, destination {dst_size} bytes)"
        ));
    }
    let src = std::slice::from_raw_parts(src, src_size);
    let dst = std::slice::from_raw_parts_mut(dst, dst_size);
    let decoded = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| match mode {
        1 => bcpack::decode(1, src, dst).map(|_| ()),
        other => Err(fail(format!(
            "mode {other} has no confirmed public block mapping"
        ))),
    }));
    match decoded {
        Ok(Ok(())) => S_OK,
        Ok(Err(hr)) => hr,
        Err(_) => fail(format!("mode {mode}: decoder panicked on a {src_size}-byte chunk")),
    }
}
