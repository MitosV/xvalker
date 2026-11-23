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

> **Scope of this table (orchestrator review):** `exec` runs americas-recomp's own RDNA2 -> SPIR-V
> translation, not the HLSL that xvalker's vendored XCL translator emits. The numbers below say how that
> other translator and `cpu_ref.py` agree on GTA's shaders; they do not validate xvalker's translator.
> Validating xvalker's HLSL against `cpu_ref.py` needs a harness that runs the dumped HLSL with the same
> bindings and inputs (not done yet).

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

## xvalker HLSL vs cpu_ref

Date: 2026-11-23. Track: `track/s4` / brief `s4-r2`.

Compared vendored XCL-translated GTA RDNA2 shaders (HLSL compiled to DXIL, run via XCL kernel ABI `io/res/mem`) against americas-recomp `cpu_ref.py` on `/tmp/opencode/s4-cmp-corpus/manifest_all.csv` (100 CS + 100 PS, binds/bins from same corpus). Source repos were read-only; work in `/tmp` copies.

### Tooling

- `/tmp/opencode/xcl-s4/crates/xcl/src/gpu/recomp/tests.rs`: env-driven ignored test `xcl_cmp_manifest` (`XCL_CMP_MANIFEST/BINS/BINDS/OUT/IDS`). Builds XCL ABI inputs from americas binds, translates RDNA2 with vendored XCL kernel options (wave64 then wave32 fallback), compiles with DXC, dispatches through `runner::run_with_statics`, dumps written data regions/images as JSONL.
- `tools/xcl-validate/s4_xcl_vs_cpuref.py`: builds americas-faithful CPU scenarios from `run_real.build`, runs `cpu_ref.py` for 100000 steps, compares data regions bit-for-bit, reports first differing dword and decoded instruction names.

### Commands

```sh
# XCL build
cd /tmp/opencode/xcl-s4
cargo test -p xcl --no-run --release --target x86_64-pc-windows-gnu

# CS XCL batches (GPU lock)
python3 /tmp/run_s4r2.py

# cpu_ref comparison
python3 /tmp/s4r2_finalize.py
# or
python3 /tmp/xcl_s4_cmp.py --xcl-jsonl /tmp/opencode/s4-r2/cs_all.jsonl \
  --manifest /tmp/opencode/s4-cmp-corpus/manifest_all.csv \
  --bins /tmp/opencode/s4-cmp-corpus/bins_all \
  --binds /tmp/opencode/s4-cmp-corpus/binds_all \
  --out /tmp/opencode/s4-r2/cs_cmp.json
```

GPU command form used by batch runner:

```sh
WINEPREFIX=~/.cache/xvalker/prefix-s4/pfx \
STEAM_COMPAT_DATA_PATH=~/.cache/xvalker/prefix-s4 \
STEAM_COMPAT_CLIENT_INSTALL_PATH=/home/mitosv/.steam/steam \
WINEDLLOVERRIDES='d3d12,d3d12core=n' WINEDEBUG=-all WINEESYNC=1 WINEFSYNC=1 \
flock /home/mitosv/.cache/xvalker/gpu.lock timeout 600 \
'/home/mitosv/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine' \
xcl-309ae8bc4b65ff3c.exe xcl_cmp_manifest --ignored --nocapture --test-threads=1
```

### Counts

| stage | equal | different | not run | total |
|---|---:|---:|---:|---:|
| CS | 1 | 40 | 59 | 100 |
| PS | 0 | 0 | 100 | 100 |

PS not run: current XCL runner path used here is compute/kernel dispatch; no pixel draw harness wired for PS in this comparison.

CS not-run reasons: cpu image access out of bounds (10), XCL GPU timeout/device removed (9), cpu read of unmapped address (6), cpu step limit (5), cpu `s_setreg_imm32_b32` (5), cpu a16/d16 image access (3), cpu MIMG opcode 0x1 (2), cpu build failure (2), remaining singletons include VOPC+SDWA, ds_write_b16, unknown encoding, LDS read before write, GDS not modelled.

### 10 most important differences

| idx | name | diff regions | first differing dword | instruction names involved (sample) |
|---|---|---:|---|---|
| 22690 | `Nflares:CS_GenOcclusionQueriesDxr_34d8d2d4` | 12 | `0x12000000 off=0 cpu=0x3fe7fa40 gpu=0x0` | `buffer_atomic_add, buffer_load_dwordx3, buffer_load_format_x, buffer_store_format_xyzw, s_add_i32, s_and_b32, s_and_saveexec_b32` |
| 23594 | `Nunified_lighting:CS_LightCulling_0f7dd029` | 8 | `0x13000000 off=0 cpu=0x3fe7fa40 gpu=0x3fce6861` | `buffer_load_dword*, buffer_load_format_x, buffer_store_dword*, image_sample_lz, s_add_i32, s_and_b32` |
| 22473 | `Nfog:CS_Volumetric_ResolveVolume_8e3d466c` | 7 | `0x13000000 off=0 cpu=0x3fe7fa40 gpu=0x0` | `buffer_atomic_add, buffer_load_dword*, buffer_store_dword, image_get_resinfo, image_load_mip, s_and_b64` |
| 22645 | `Nunified_lighting:CS_LightCulling_4720d5e5` | 7 | `0x12000000 off=0 cpu=0x3fe7fa40 gpu=0x3fce6861` | `image_gather4_lz, image_load, image_sample_lz, image_store, s_and_saveexec_b32, s_brev_b32` |
| 22623 | `Nindirect_diffuse_lighting:CS_ScreenProbes_GenerateShaderBins_f54cc70b` | 6 | `0x12000000 off=0 cpu=0x3fe7fa40 gpu=0x0` | `buffer_atomic_add, buffer_load_format_x, buffer_store_format_xyzw, ds_read2_b32, ds_read_b32, ds_write_b32` |
| 24237 | `Nssao:CS_SSAO_20c6b84a` | 6 | `0x12000000 off=0 cpu=0x3fe7fa40 gpu=0x3fce6861` | `buffer_atomic_add, buffer_load_format_x, buffer_store_format_xyzw, ds_add_rtn_u32, ds_read_b32, image_load` |
| 24303 | `Nvolumetric_clouds:CS_CloudShadow_4ca0f06f` | 6 | `0x11000000 off=0 cpu=0x3fe7fa40 gpu=0x0` | `buffer_load_format_x, image_load, image_store, s_and_saveexec_b64, s_buffer_load_dwordx4, s_cbranch_execz` |
| 22235 | `Nparticles:CS_Simulate_2bd58c4e` | 5 | `0x13000000 off=0 cpu=0x3fe7fa40 gpu=0x3fce6861` | `buffer_load_dword*, buffer_store_dwordx3, buffer_store_format_xyzw, s_and_saveexec_b32, s_andn2_b32` |
| 23335 | `Nunified_lighting:CS_LightCulling_473a2385` | 5 | `0x12000000 off=0 cpu=0x3fe7fa40 gpu=0x0` | `buffer_load_format_xyz, image_load, image_sample_lz, image_store, s_and_saveexec_b64, s_andn2_b64` |
| 24231 | `Nssao:CS_SSAO_00a7f9af` | 5 | `0x12000000 off=0 cpu=0x3fe7fa40 gpu=0x0` | `buffer_load_format_x, image_gather4_lz, image_load, image_store, s_and_saveexec_b64, s_cbranch_execz` |

### Notes

- Only 1 of 41 CS shaders comparable by both sides was bit-equal; 40 differed. Result is **not validation** of xvalker HLSL path for this corpus.
- Differences may come from T#/image ABI mapping, kernel SGPR/layout, memory window behavior, or translation/runtime semantics. First-dword attribution is coarse; instruction names are shader-context, not proven causal PCs.
- Artifacts: `/tmp/opencode/s4-r2/cs_all.jsonl`, `/tmp/opencode/s4-r2/cs_cmp.json`, `/tmp/opencode/s4-r2/run.log`, report `~/.cache/xvalker/state/reports/s4-r2.md`.

## s4-r3: identical inputs on both sides

Date: 2026-11-23. The s4-r2 table above is **superseded**: the GPU side rebuilt memory
from binds with different bases and a different PRNG than `run_real.build`, so most
"differences" were input mismatches (first dword `0x3fe7fa40` on CPU vs `0x0` on GPU).

`tools/xcl-validate/` now has one scenario builder (`build_scenarios.py`) that calls
`run_real.build`, compacts all guest regions, rewrites pointers/V#/T#, and serializes
code + 128 SGPRs + 32x256 VGPRs + all regions + all images + FNV-1a hashes into one
JSON file per shader. Both `cpu_ref.py` and XCL's `xcl_cmp_manifest` test load that
file; no input is rebuilt on the GPU side. `cpu_ref.py` is patched to use 32 lanes and
to decode XCL's SRV-index T# (patches and exact commands are in
`tools/xcl-validate/README.md`).

### Input self-check and canary

- 100/100 CS shaders: code, SGPR, VGPR, every region and every image hash equal on both sides; **0 input mismatches**.
- Trivial `BUFFER_LOAD_DWORD` + `BUFFER_STORE_DWORD` canary: both sides stored `0x12345678` — equal.

### New CS counts (same 100 shaders, both sides from the serialized scenario)

| stage | equal | different | not run | total |
|---|---:|---:|---:|---:|
| CS | 32 | 1 | 67 | 100 |

Not-run reasons: XCL GPU timeout/device removed 23, XCL `cannot decode` 8,
cpu_ref unsupported 36 (image bounds 7, image dim mismatch 6, step limit 4,
a16/d16 image 3, `s_setreg_imm32_b32` 2, MIMG opcode 2, other 12).

### Remaining difference traced to its first instruction

| idx | name | first differing instruction | first differing register |
|---|---|---|---|
| 23465 | `7sky_system:CS_AmbientCubeIBRLightning_e7f30445` | #15, byte `0x54`, `v_fmac_f32` (full program: 25 instructions) | `v1[0]`: cpu `0x40307531` vs gpu `0x3d64e039` |

Prefixes 1..14 were bit-identical in output and written registers; prefix 15 diverges.
This is now a real translator/runtime candidate, not an input mismatch. Artifacts:
`/tmp/opencode/s4-r3/{scenarios,gpu_all.jsonl,cmp_all.json,bisect.json,copy.jsonl}`.
