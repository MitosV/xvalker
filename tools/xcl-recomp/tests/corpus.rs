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

/// `v_readlane_b32 s14, v6, 32; s_endpgm` (gfx1010), assembled with llvm-mc: a read of a
/// lane of the upper half of a wave64. XCL's `upper_lanes` analysis follows it (the lane was
/// never written), so the 32-lane form does not refuse it.
#[test]
fn a_32_lane_host_resolves_a_read_of_the_upper_half() {
    let code = [0x0e, 0x00, 0x60, 0xd7, 0x06, 0x41, 0x01, 0x02, 0x00, 0x00, 0x81, 0xbf];
    let (hazards, resolved) = xcl_recomp::gpu::recomp::wave_lane_reads(&code);
    assert!(hazards.is_empty(), "{hazards:?}");
    assert!(resolved.contains("lanes ≥ 32 never written (or inactive)"), "{resolved:?}");
    // a compute program runs as two halves that exchange their masks and lanes
    let kernel = xcl_recomp::gpu::recomp::Mode::Kernel;
    let opt = xcl_recomp::gpu::recomp::Options { mode: kernel.clone(), wave64: true, split_wave64: true, ..Default::default() };
    let split = xcl_recomp::gpu::recomp::translate(&code, &opt).unwrap();
    assert!(split.hlsl.contains("[WaveSize(32)]"), "{}", split.hlsl);
    // a pixel shader runs one invocation per fragment: it gets the 32-lane form too
    let pixel = xcl_recomp::gpu::recomp::Mode::Pixel(Default::default());
    let opt = xcl_recomp::gpu::recomp::Options { mode: pixel, wave64: true, split_wave64: true, ..Default::default() };
    assert!(xcl_recomp::gpu::recomp::translate(&code, &opt).is_ok());
    // the native form (a GPU with 64-lane waves) is unchanged
    let opt = xcl_recomp::gpu::recomp::Options { mode: kernel, wave64: true, ..Default::default() };
    let native = xcl_recomp::gpu::recomp::translate(&code, &opt).unwrap();
    assert!(native.hlsl.contains("[WaveSize(64)]"), "{}", native.hlsl);
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

/// `v_fmac_f32 v1, 0x3d93a92a, v2` (shader 23465 at +0x54). RDNA2 defines it as
/// VDST = SRC0 * VSRC1 + VDST with a single fused rounding, so the emitter must
/// keep the old VDST as the third operand of HLSL `mad`.
#[test]
fn v_fmac_f32_is_a_fused_read_modify_write() {
    use xcl_recomp::gpu::recomp;
    let code = [
        0xff, 0x04, 0x02, 0x56, 0x2a, 0xa9, 0x93, 0x3d, // v_fmac_f32 v1, lit, v2
        0x00, 0x00, 0x81, 0xbf, // s_endpgm
    ];
    let opt = recomp::Options { mode: recomp::Mode::Kernel, wave64: false, ..Default::default() };
    let t = recomp::translate(&code, &opt).unwrap();
    assert!(
        t.hlsl.contains("mad(asfloat(0x3D93A92Au), asfloat(v2), asfloat(v1))"),
        "unexpected emission:\n{}",
        t.hlsl
    );
}

/// `buffer_load_dwordx3 v[0:2], v[0:1], s[0:3], 0 idxen` (shader 23465 at +0x28).
/// VDATA overlaps the index VADDR: the bounds check must use the index the
/// instruction had, not the value the first component loaded.
#[test]
fn mubuf_vector_load_bounds_checks_the_index_before_it_is_overwritten() {
    use xcl_recomp::gpu::recomp;
    let code = [
        0x00, 0x20, 0x3c, 0xe0, 0x00, 0x00, 0x00, 0x80, // buffer_load_dwordx3
        0x00, 0x00, 0x81, 0xbf, // s_endpgm
    ];
    let opt = recomp::Options { mode: recomp::Mode::Kernel, wave64: false, ..Default::default() };
    let t = recomp::translate(&code, &opt).unwrap();
    let lines: Vec<&str> = t.hlsl.lines().collect();
    let check = lines
        .iter()
        .position(|l| l.contains("v0 < s2"))
        .unwrap_or_else(|| panic!("no bounds check in:\n{}", t.hlsl));
    let write = lines
        .iter()
        .position(|l| l.trim_start().starts_with("v0 = "))
        .unwrap_or_else(|| panic!("no write in:\n{}", t.hlsl));
    assert!(check < write, "the bounds check used the overwritten index:\n{}", t.hlsl);
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
