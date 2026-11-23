# XCL translator GPU validation (s4-r1)

GPU: NVIDIA GeForce RTX 4070 Ti (wave32 only). All GPU runs used
`flock ~/.cache/xvalker/gpu.lock timeout <s> <cmd>` and
`WINEPREFIX=~/.cache/xvalker/prefix-s4/pfx`. Work was done in copies under `/tmp`; the
read-only source repositories were not modified.

## 1. Upstream XCL's own GPU tests

Source: `~/dev/projects/xcl` at `dd713e3` (the copy also carries that repo's pre-existing
uncommitted `tls.rs` Wine workaround). Build:

```sh
cp -a ~/dev/projects/xcl /tmp/opencode/xcl-s4
cd /tmp/opencode/xcl-s4
cargo test --no-run --release --target x86_64-pc-windows-gnu -p xcl
# copied d3d12.dll, d3d12core.dll, dxcompiler.dll, dxil.dll from main's build/linux
# next to target/x86_64-pc-windows-gnu/release/deps/xcl-309ae8bc4b65ff3c.exe
```

Run (Proton wine, native vkd3d-proton DLLs):

```sh
cd target/x86_64-pc-windows-gnu/release/deps
WINEPREFIX=~/.cache/xvalker/prefix-s4/pfx \
STEAM_COMPAT_DATA_PATH=~/.cache/xvalker/prefix-s4 \
STEAM_COMPAT_CLIENT_INSTALL_PATH=~/.steam/steam \
WINEDLLOVERRIDES='d3d12,d3d12core=n' WINEDEBUG=-all WINEESYNC=1 WINEFSYNC=1 \
flock ~/.cache/xvalker/gpu.lock timeout 1800 \
'/home/mitosv/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine' \
xcl-309ae8bc4b65ff3c.exe gpu::recomp --test-threads=1 --nocapture
```

Result: **99 passed, 0 failed, 13 ignored, 245 filtered out**. Of the 99 "passed",
**41 actually printed `SKIPPED <test>: no wave64`**; the remaining 58 were real passes,
including the wave32-capable geometry/mesh tests. Wave64-on-wave32 failures: **0** – the
harness skips them instead of reporting failures. The 41 skips (all with reason
`no wave64`) are: `far_jumps_are_direct_branches`, `gpu_atomic_fcmpswap_x2`,
`gpu_atomic_fmin_fmax_negative_zero`, `gpu_atomic_fmin_fmax_x2`, `gpu_bench_really_runs`,
`gpu_built_bvh_nodes_match_the_cpu_node_test`, `gpu_bvh64_intersect_ray_across_memory_windows_and_grazing`,
`gpu_bvh_intersect_ray`, `gpu_compute_mode_dispatch_abi`, `gpu_divergence_through_exec`,
`gpu_ds_read_destination_overlaps_address`, `gpu_ds_swizzle_modes`,
`gpu_dword_addtid_per_lane_addresses`, `gpu_f64_class_compare`,
`gpu_format_stores_atomics_global_lds`, `gpu_frexp_f64_exp_and_mant`,
`gpu_geometry_wave32_prefix_count_over_a_mask`,
`gpu_global_atomics_and_sub_dword_access`, `gpu_graphics_vs_ps_draw`,
`gpu_images_a16_d16`, `gpu_image_sample_offset`, `gpu_images_sample_load_store_atomic`,
`gpu_memory_and_exports`, `gpu_memory_windows_above_4_gib`, `gpu_mesh_mode_two_passes`,
`gpu_more_atomics_sub_dword_scratch_valu`, `gpu_packed_math_dots_movrel`, `gpu_rndne_f64`,
`gpu_scalar_loop_and_scc`, `gpu_scalar_relative_moves`, `gpu_sdwa_dpp_and_cross_lane`,
`gpu_sparse_vram_descriptors_buffers_and_xmem`, `gpu_static_samplers_from_constants_in_code`,
`gpu_triangle_pair_and_box16_nodes`, `gpu_tsharp_to_d3d12_resources`,
`gpu_typed_buffer_loads_and_stores`, `gpu_unified_memory_arena`, `gpu_valu_16_and_64_bit`,
`gpu_valu_float_and_int`, `gpu_v_swaprel_b32`, `gpu_wave32_program_in_wave64_hardware`.
The geometry-wave32 test prints only the `SKIPPED` line; the other 40 also print
`GPU/DXC wave64 unavailable – skipping (None / Some(false) / None)` first. All 41
`SKIPPED` lines use reason `no wave64`. There were no failed test names or messages.

## 2. Vendored copy

`diff` against `git show dd713e3:crates/xcl/src/...` for all 21 vendored `.rs` files:
14 byte-identical, 7 different. The differences are exactly the ones documented in
`third_party/xcl/PATCHES.md`: `recomp/mod.rs` (four module declarations removed:
`dxc`, `pso`, `runner`, `tests`), `rdna2.rs` / `recomp/mesh_dispatch.rs` (test modules
behind `xcl_vendored_tests`), `gpu/shader.rs` (Windows capture code behind
`cfg(xcl_runtime)` plus one `crate::dxc` call-site change), and `shaders.rs`, `rpf.rs`,
`util.rs` (Linux build paths, outside the translator).
No translation logic differs; `gpu/recomp/emit.rs`, `gpu/recomp/mesh_wave.rs`,
`gpu/dxbc.rs`, `gpu/pack.rs`, `gpu/rdna2_spec.rs`, `gpu/rdna2_xbox.rs`, `gpu/xbox.rs`
are byte-identical.

To be exhaustive rather than relying on that inspection, a second copy of XCL was built
with the vendored translator files copied over the upstream ones (restoring the three
Windows-only `recomp` module declarations and the `cfg(windows)` gate needed by XCL's own
Windows crate). The same GPU test filter gave **94 passed, 0 failed, 13 ignored,
233 filtered out, 41 `SKIPPED ... no wave64`**. The five-test difference is exactly the
five `gpu::recomp::mesh_dispatch::tests` disabled by that file's `xcl_vendored_tests`
gate; the gates in `rdna2.rs` and `gpu/shader.rs` affect tests outside this filter. No
semantic difference was observed. Step 1 therefore covers the vendored translator.

## 3. Real shaders against the CPU reference

* `carve_shaders.py /mnt/games/GTA/GTA6.Xbox.v1.0.2.1/shaders.rpf --out /tmp/opencode/s4-corpus`:
  26,503 records; 2,689 CS and 12,101 PS blobs.
* New option (commit `04dc0fe`): `xcl-corpus <pack> --dump-hlsl DIR --limit N --stage cs|ps`.
  Dumps of 100 CS and 100 PS translated programs completed with 0 translate errors
  (106 / 107 records scanned due to parallel reservation). The CPU reference harness
  consumes raw RDNA2 blobs, not HLSL, so these dumps are a translation cross-check, not
  GPU input.
* A deterministic sample of 100 CS and 100 PS shaders (<= 8000 bytes each, seed 42026)
  was carved into `bins_all`/`binds_all`/`manifest_all.csv`; `bindings.py` wrote 200 fresh
  bindings (version `adf1bbf4edbc`). `exec` was built from a copy of `americas-recomp`
  with local SPIRV/Vulkan headers; `exec_smoke.py` passed.
* Commands: `XBOX_RDNA2=recomp XBOX_CORPUS_DIR=... XBOX_BINS_DIR=... XBOX_BINDS_DIR=...
  XBOX_MANIFEST=... XBOX_EXEC=/tmp/opencode/americas-s4/build-xbox/exec
  XBOX_EXEC_PS=<same> flock ~/.cache/xvalker/gpu.lock timeout 3600 python3 -I
  tools/xbox/cpu_compare{,_ps}.py /tmp/opencode/s4-{cs,ps}.json --ids <100 ids>
  --steps 100000 --gpu-timeout 30 --workers 8 --no-canary` (the copied
  `cpu_compare_ps.py` uses `/usr/bin/glslangValidator` instead of its hard-coded Windows
  path).

| stage | equal (MATCH) | different (MISMATCH) | not run (SKIP) | not run (GPU_FAIL) |
|---|---:|---:|---:|---:|
| CS | 30 (15 changed memory) | 1 | 58 | 11 |
| PS | 23 | 10 | 65 | 2 |
| total | 53 | 11 | 123 | 13 |

All 13 GPU_FAILs are the harness rejection `FAIL AGC graphics: guest sampler descriptor
uses point preclamping which is not implemented`; common SKIP reasons include image bounds,
`a16/d16` image access, `s_setreg_imm32_b32`, step limit and 16-bit interpolants.

Ten most important mismatches (shader idx, first differing value, distinguishing opcodes):

| idx | first difference (GPU vs CPU) | instruction names |
|---|---|---|
| PS 26000 | `0x40032000` vs `0x3f9aa000` | `v_interp_p1_f32`, `v_interp_p2_f32`, `v_mul_legacy_f32`, `v_cvt_pkrtz_f16_f32` |
| PS 25818 | `0x3f800000` vs `0x3f1f4000` | `v_interp_p1_f32`, `v_interp_p2_f32`, `v_fmac_f32`, `v_med3_f32` |
| PS 26301 | `0x0` vs `0x3fcc2000` | `v_interp_p1_f32`, `v_interp_p2_f32`, `v_fmac_f32`, `v_med3_f32` |
| PS 11305 | `0xc4424000` vs `0x0` at pixel 33 | `exp`, `v_cndmask_b32`, `v_cmp_eq_u32`, `v_cvt_pkrtz_f16_f32` |
| PS 25836 | `0x3f596000` vs `0x408d8000` | `v_fmac_f32`, `v_med3_f32`, `v_mul_legacy_f32`, `v_interp_p1_f32` |
| PS 25973 | `0x3f800000` vs `0x3f784000` | `v_sub_f32`, `v_fmac_f32`, `v_med3_f32`, `v_interp_p1_f32` |
| PS 25618 | `0x3fb4c000` vs `0x3eb84000` | `v_mul_legacy_f32`, `v_fmac_f32`, `v_med3_f32`, `v_interp_p1_f32` |
| PS 25500 | `0x40946000` vs `0x40284000` | `v_med3_f32`, `v_sub_f32`, `v_mul_legacy_f32`, `v_interp_p1_f32` |
| PS 25650 | `0x3f282000` vs `0x405ae000` | `v_fmac_f32`, `v_med3_f32`, `v_cvt_pkrtz_f16_f32`, `v_interp_p1_f32` |
| CS 22623 | `0x0` vs `0x1f0000` in region 203 | `v_min_u32`, `v_cmpx_eq_u32`, `v_cmp_lt_u32`, `v_add_nc_u32` |

Caveat: on this wave32 GPU, wave64 compute shaders run as two 32-wide halves in
`exec`; the CPU reference models a logical wave64. The GPU side here is
`americas-recomp`'s Vulkan recompiler, not XCL's emitted HLSL, so these numbers validate
the shader results against `cpu_ref.py`, not the XCL translator's DXIL directly.
