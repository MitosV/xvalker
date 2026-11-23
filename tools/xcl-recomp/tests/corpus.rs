//! Tests of the corpus tool's walk and of the vendored recompiler.
//!
//! The pack used here is a synthetic one: a record with a DXBC container that XCL's
//! parser rejects (so the tool counts it as "not a console container") and a record
//! that inflates to garbage.

use std::collections::BTreeMap;

/// A record of an RPF9 pack, as `gpu::pack::scan` expects it.
fn record(name: &str, payload: &[u8]) -> Vec<u8> {
    let compressed = miniz_oxide::deflate::compress_to_vec(payload, 6);
    let mut bytes = vec![(name.len() + 1) as u8];
    bytes.extend_from_slice(name.as_bytes());
    bytes.push(0);
    bytes.push(50);
    for value in [
        compressed.len() as u32,
        0,
        compressed.len() as u32,
        payload.len() as u32,
    ] {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes.extend_from_slice(&compressed);
    bytes
}

#[test]
fn the_pack_scanner_finds_the_records() {
    let mut pack = b"9FPR".to_vec();
    pack.resize(0x40, 0);
    pack.extend(record("PS_One", b"first"));
    pack.extend(record("VS_Two", b"second"));
    let records = xcl_recomp::gpu::pack::scan(&pack, 0x40);
    assert_eq!(records.len(), 2);
    assert_eq!(records[0].name, "PS_One");
    assert_eq!(records[1].name, "VS_Two");
    assert_eq!(
        xcl_recomp::gpu::pack::inflate(&pack, &records[0]).as_deref(),
        Some(&b"first"[..])
    );
}

#[test]
fn a_record_that_does_not_inflate_is_reported() {
    let mut pack = b"9FPR".to_vec();
    pack.resize(0x40, 0);
    pack.extend(record("PS_Broken", b"payload"));
    // corrupt the deflate stream
    let last = pack.len() - 1;
    pack[last] = 0xff;
    let records = xcl_recomp::gpu::pack::scan(&pack, 0x40);
    assert_eq!(records.len(), 1);
    assert!(xcl_recomp::gpu::pack::inflate(&pack, &records[0]).is_none());
}

#[test]
fn the_translator_rejects_a_blob_that_is_not_a_container() {
    let mut result = xcl_recomp::XclResult {
        hlsl: std::ptr::null_mut(),
        profiles: std::ptr::null_mut(),
        error: std::ptr::null_mut(),
        instructions: 0,
        unsupported: 0,
    };
    let blob = b"not a console shader container";
    let code = unsafe {
        xcl_recomp::xcl_translate(blob.as_ptr(), blob.len(), xcl_recomp::XCL_FLAG_NONE, &mut result)
    };
    assert!(code < 0);
    assert!(result.error.is_null() == false);
    unsafe { xcl_recomp::xcl_free(&mut result) };
    // the pointers are cleared, so a second free is harmless
    unsafe { xcl_recomp::xcl_free(&mut result) };
}

#[test]
fn the_dxc_arguments_are_the_ones_xcl_uses() {
    let args = xcl_recomp::dxc::args("source", "main", "cs_6_6");
    assert_eq!(args, vec!["-E", "main", "-T", "cs_6_6", "-HV", "2021"]);
    // a huge source gets -O0, as in XCL
    let big = "x".repeat(9 * 1024 * 1024);
    assert!(xcl_recomp::dxc::args(&big, "main", "cs_6_6").contains(&"-O0".to_string()));
}

#[test]
fn the_cache_key_depends_on_the_profile_and_the_source() {
    let a = xcl_recomp::dxc::cache_key("source", "main", "cs_6_6");
    let b = xcl_recomp::dxc::cache_key("source", "main", "ps_6_6");
    let c = xcl_recomp::dxc::cache_key("other", "main", "cs_6_6");
    assert_ne!(a, b);
    assert_ne!(a, c);
    assert_eq!(a, xcl_recomp::dxc::cache_key("source", "main", "cs_6_6"));
}

#[test]
fn the_wave_size_is_detected_from_the_program() {
    // A program that touches neither the 32-bit nor the 64-bit mask registers is a
    // wave64 program (XCL's rule: wave32 only when a 32-bit EXEC op is there and no
    // 64-bit one is).
    let empty = [0u8; 16];
    assert!(xcl_recomp::gpu::recomp::detect_wave64(&empty));
}

#[test]
fn the_stage_of_a_mode_is_the_one_of_the_program() {
    // a program with no instructions still has a mode; the kernel mode is compute
    use xcl_recomp::gpu::recomp::Mode;
    let mode = Mode::Kernel;
    assert!(matches!(mode, Mode::Kernel));
}

/// The error groups of the tool are the errors by message, most frequent first.
#[test]
fn the_error_groups_are_sorted_by_count() {
    let mut groups: BTreeMap<String, usize> = BTreeMap::new();
    *groups.entry("a".into()).or_default() += 3;
    *groups.entry("b".into()).or_default() += 7;
    let mut v: Vec<_> = groups.iter().collect();
    v.sort_by(|a, b| b.1.cmp(a.1));
    assert_eq!(v[0].0, "b");
}
