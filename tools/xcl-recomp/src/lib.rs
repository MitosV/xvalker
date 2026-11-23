//! XCL's console shader recompiler, vendored from the XCL project (see
//! `third_party/xcl/README.md`) and wrapped in a C ABI.
//!
//! The recompiler translates console RDNA2 microcode (as found in Xbox Series X|S
//! shader packs) into HLSL that DXC can compile. It is a pure translator: it needs
//! neither a GPU nor Windows.
//!
//! # C ABI
//!
//! ```c
//! #include "xcl_recomp.h"
//!
//! XclResult result;
//! if (xcl_translate(container, container_size, XCL_FLAG_NONE, &result) == 0) {
//!     // result.hlsl holds the translated HLSL (UTF-8, NUL-terminated)
//!     xcl_free(&result);
//! }
//! ```
//!
//! `xcl_translate` never returns a null pointer in `out`: on failure the result holds
//! the error text and the returned code is negative.

#![allow(non_camel_case_types)]

use std::ffi::{c_char, CString};

pub mod gpu;

#[path = "../../../third_party/xcl/src/rpf_probe.rs"]
pub mod rpf_probe;

#[path = "../../../third_party/xcl/src/rpf.rs"]
pub mod rpf;

#[path = "../../../third_party/xcl/src/shaders.rs"]
pub mod shaders;

pub mod dxc;

pub use gpu::recomp;

#[cfg(test)]
mod tests;

/// Flags for [`xcl_translate`].
pub const XCL_FLAG_NONE: u32 = 0;
/// Force wave64 semantics even when the program looks like a wave32 program.
pub const XCL_FLAG_FORCE_WAVE64: u32 = 1 << 0;
/// Force wave32 semantics even when the program looks like a wave64 program.
pub const XCL_FLAG_FORCE_WAVE32: u32 = 1 << 1;
/// The host GPU runs 32-lane waves only (see the header).
pub const XCL_FLAG_HOST_WAVE32: u32 = 1 << 2;

/// The result of a translation. Every pointer is owned by the result and released by
/// [`xcl_free`].
#[repr(C)]
pub struct XclResult {
    /// The translated HLSL (UTF-8, NUL-terminated). NULL on failure.
    pub hlsl: *mut c_char,
    /// The DXC profile(s) the HLSL needs, comma separated (e.g. "vs_6_6"). NULL on failure.
    pub profiles: *mut c_char,
    /// The error text (UTF-8, NUL-terminated). NULL on success.
    pub error: *mut c_char,
    /// The number of instructions the program has.
    pub instructions: u64,
    /// The number of instructions the translator could not translate.
    pub unsupported: u64,
}

impl XclResult {
    fn empty() -> Self {
        XclResult {
            hlsl: std::ptr::null_mut(),
            profiles: std::ptr::null_mut(),
            error: std::ptr::null_mut(),
            instructions: 0,
            unsupported: 0,
        }
    }
}

/// Translate one console shader container (an Xbox shader blob, as found in a shader
/// pack) into HLSL.
///
/// `container` is the whole console container (`gpu::xbox::XboxShader`), `size` its
/// length. On success 0 is returned and `out` holds the HLSL and its profiles; on
/// failure a negative value is returned and `out` holds the error text.
///
/// # Safety
/// `container` must point to `size` readable bytes; `out` must point to a writable
/// `XclResult`.
#[no_mangle]
pub unsafe extern "C" fn xcl_translate(container: *const u8, size: usize, flags: u32, out: *mut XclResult) -> i32 {
    if out.is_null() {
        return -1;
    }
    let result = &mut *out;
    *result = XclResult::empty();
    if container.is_null() || size == 0 {
        result.error = c_string("empty container");
        return -1;
    }
    let blob = std::slice::from_raw_parts(container, size);

    let Some(sh) = gpu::xbox::parse(blob) else {
        result.error = c_string("not a console shader container");
        return -2;
    };
    let mut translated: Option<(String, Vec<String>)> = None;
    let mut first_error: Option<String> = None;
    for f in &sh.fssm {
        let Some(mode) = gpu::recomp::interface(blob, &sh, f) else { continue };
        for p in &f.programs {
            let code = &f.data[p.offset..p.offset + p.len];
            let wave64 = if flags & XCL_FLAG_FORCE_WAVE32 != 0 {
                false
            } else if flags & XCL_FLAG_FORCE_WAVE64 != 0 {
                true
            } else {
                gpu::recomp::detect_wave64(code)
            };
            // a 32-lane host runs a wave64 program as two native wave32 halves (as xcl-corpus
            // --host-wave 32 does)
            let host_wave = if flags & XCL_FLAG_HOST_WAVE32 != 0 { 32 } else { 0 };
            let opt = gpu::recomp::Options {
                mode: mode.clone(),
                wave64,
                hw_wave: host_wave,
                geometry_wave32: host_wave == 32 && matches!(mode, gpu::recomp::Mode::Geometry(_)),
                mesh_wave32: host_wave == 32 && matches!(mode, gpu::recomp::Mode::Mesh(_)),
                host_wave,
                ..Default::default()
            };
            match gpu::recomp::translate(code, &opt) {
                Ok(t) => {
                    let profiles = match &mode {
                        gpu::recomp::Mode::Vertex(_) => vec!["vs_6_6".to_string()],
                        gpu::recomp::Mode::Pixel(_) => vec!["ps_6_6".to_string()],
                        gpu::recomp::Mode::Compute(_) => vec!["cs_6_6".to_string()],
                        gpu::recomp::Mode::Mesh(_) => vec!["cs_6_6".to_string(), "ms_6_6".to_string()],
                        gpu::recomp::Mode::Geometry(_) => vec!["cs_6_6".to_string()],
                        gpu::recomp::Mode::Kernel => vec!["cs_6_6".to_string()],
                    };
                    result.instructions = t.instructions as u64;
                    result.unsupported = t.unsupported.values().sum::<usize>() as u64;
                    translated = Some((t.hlsl, profiles));
                    break;
                }
                Err(e) => {
                    if first_error.is_none() {
                        first_error = Some(e);
                    }
                }
            }
        }
        if translated.is_some() {
            break;
        }
    }
    match translated {
        Some((hlsl, profiles)) => {
            result.hlsl = c_string(&hlsl);
            result.profiles = c_string(&profiles.join(","));
            0
        }
        None => {
            result.error = c_string(first_error.as_deref().unwrap_or("no translatable program in the container"));
            -3
        }
    }
}

/// Release the strings of a result. Safe to call with a null pointer.
///
/// # Safety
/// `result` must point to a result that was filled by [`xcl_translate`].
#[no_mangle]
pub unsafe extern "C" fn xcl_free(result: *mut XclResult) {
    if result.is_null() {
        return;
    }
    let r = &mut *result;
    for p in [&mut r.hlsl, &mut r.profiles, &mut r.error] {
        if !p.is_null() {
            drop(CString::from_raw(*p));
            *p = std::ptr::null_mut();
        }
    }
}

fn c_string(s: &str) -> *mut c_char {
    std::ffi::CString::new(s).unwrap_or_default().into_raw()
}
