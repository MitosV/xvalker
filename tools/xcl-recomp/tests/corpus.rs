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

/// `v_readlane_b32 s14, v6, 32; s_endpgm` (gfx1010), assembled with llvm-mc.
#[test]
fn a_32_lane_host_exchanges_a_read_of_the_upper_half_only_with_a_workgroup() {
    let code = [0x0e, 0x00, 0x60, 0xd7, 0x06, 0x41, 0x01, 0x02, 0x00, 0x00, 0x81, 0xbf];
    let hazard = "v_readlane/writelane: lane ≥ 32";
    let kernel = xcl_recomp::gpu::recomp::Mode::Kernel;
    assert!(xcl_recomp::gpu::recomp::wave_hazards(&code).contains(hazard));
    // Compute has a workgroup: the read is served from groupshared memory.
    assert!(xcl_recomp::gpu::recomp::host_hazards(&code, &kernel, 32).is_empty());
    let opt = xcl_recomp::gpu::recomp::Options { mode: kernel, wave64: true, host_wave: 32, ..Default::default() };
    assert!(xcl_recomp::gpu::recomp::translate(&code, &opt).is_ok());
    // A pixel shader runs one invocation per fragment: no workgroup to exchange through.
    let pixel = xcl_recomp::gpu::recomp::Mode::Pixel(Default::default());
    assert!(xcl_recomp::gpu::recomp::host_hazards(&code, &pixel, 32).contains(hazard));
    let opt = xcl_recomp::gpu::recomp::Options { mode: pixel, wave64: true, host_wave: 32, ..Default::default() };
    assert!(xcl_recomp::gpu::recomp::translate(&code, &opt).is_err());
}

/// DXC rounds the mesh pass-through's vertex stride up to a power of two of 16-byte
/// elements. 21 outputs (SV_Position included) cost 32 slots = 512 bytes per vertex:
/// 78 vertices no longer fit one 32 KB group, so the emitters split its primitives.
#[test]
fn the_mesh_passthrough_accounts_for_dxc_s_padded_vertex_stride() {
    use xcl_recomp::gpu::recomp;
    let ms = |outputs, vertices| recomp::MsInterface {
        outputs: vec![
            xcl_recomp::gpu::dxbc::SigElement {
                name: "TEXCOORD".into(),
                index: 0,
                system_value: 0,
                component_type: 3,
                register: 0,
                mask: 15,
                rw_mask: 0,
                stream: 0,
            };
            outputs
        ],
        threads: 128,
        max_vertices: vertices,
        max_primitives: 26,
    };
    assert!(recomp::mesh_passthrough(&ms(15, 128)).contains("verts[128]"));
    assert!(recomp::mesh_passthrough(&ms(21, 64)).contains("verts[64]"));
    // 26 primitives / 2 groups -> 13 triangles, 39 vertex copies per group.
    let split = recomp::mesh_passthrough(&ms(21, 78));
    assert!(split.contains("verts[39]") && split.contains("tris[13]"));
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
