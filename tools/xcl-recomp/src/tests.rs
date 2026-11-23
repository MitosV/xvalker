//! Unit-level check of the translation path used for console compute shaders.
//!
//! A synthetic RDNA2 compute container exercises `xcl_translate` without a game: the ABI
//! buffers (`io`, `res`) come from the recompiler, and DXC must accept the resulting HLSL.

use crate::gpu::{xbox, xbox::tests as build};

#[test]
fn synthetic_compute_container_translates_and_compiles() {
    // s_mov_b32 s0, s1 ; v_add_f32 v0, v1, v5 ; s_endpgm
    let mut fssm = build::fssm(&build::CODE, &[(6, 0)]);
    // COMPUTE_PGM_RSRC2 and numthreads in the stage descriptor at +0x100.
    for (at, value) in [(0x138, 0u32), (0x13c, 64), (0x140, 1), (0x144, 1)] {
        fssm[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }
    let container = build::sample(5, &[fssm]);
    assert!(xbox::is_xbox(&container));

    let mut result = crate::XclResult::empty();
    let status = unsafe { crate::xcl_translate(container.as_ptr(), container.len(), crate::XCL_FLAG_NONE, &mut result) };
    assert_eq!(status, 0, "{}", unsafe { std::ffi::CStr::from_ptr(result.error).to_string_lossy() });
    let hlsl = unsafe { std::ffi::CStr::from_ptr(result.hlsl).to_string_lossy().into_owned() };
    let profiles = unsafe { std::ffi::CStr::from_ptr(result.profiles).to_string_lossy().into_owned() };
    assert_eq!(profiles, "cs_6_6");
    assert!(hlsl.contains("RWByteAddressBuffer io : register(u0);"), "{hlsl}");
    assert!(hlsl.contains("RWByteAddressBuffer res : register(u1);"), "{hlsl}");
    unsafe { crate::xcl_free(&mut result) };

    if std::env::var_os("XCL_DXC").is_none() {
        println!("XCL_DXC is not set: DXC compile skipped");
        return;
    }
    let dxil = crate::dxc::compile(&hlsl, "main", "cs_6_6", &[]).expect("DXC compile");
    assert!(dxil.starts_with(b"DXBC") && dxil.len() > 1024, "expected a DXIL container, got {} bytes", dxil.len());
}
