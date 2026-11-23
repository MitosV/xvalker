# XCL's shader recompiler (vendored)

XCL (<https://github.com/> the XCL project, by the author of this project) translates
console RDNA2 shaders to HLSL. This directory holds the parts of XCL that do that
translation, so they can be built and used outside XCL (in XValkyrie's
`tools/xcl-recomp` crate) without a Windows host.

The author of XCL owns this project: copying XCL code is allowed.

## Source

| | |
|---|---|
| Repository | XCL (`~/dev/projects/xcl`) |
| Commit | `dd713e3` ("Merge pull request #52 from gefydev/xml-attr-entities") |
| Path in XCL | `crates/xcl/src/` |

## Files

All files are copies of `crates/xcl/src/<path>` of the commit above. The ones marked
*edited* differ; every edit is listed in `PATCHES.md`.

| File | Origin in XCL | Edited |
|---|---|---|
| `src/gpu/rdna2.rs` | `crates/xcl/src/gpu/rdna2.rs` | no |
| `src/gpu/rdna2_spec.rs` | `crates/xcl/src/gpu/rdna2_spec.rs` | no |
| `src/gpu/rdna2_xbox.rs` | `crates/xcl/src/gpu/rdna2_xbox.rs` | no |
| `src/gpu/xbox.rs` | `crates/xcl/src/gpu/xbox.rs` | no |
| `src/gpu/shader.rs` | `crates/xcl/src/gpu/shader.rs` | yes |
| `src/gpu/dxbc.rs` | `crates/xcl/src/gpu/dxbc.rs` | no |
| `src/gpu/pack.rs` | `crates/xcl/src/gpu/pack.rs` | no |
| `src/gpu/recomp/mod.rs` | `crates/xcl/src/gpu/recomp/mod.rs` | yes |
| `src/gpu/recomp/emit.rs` | `crates/xcl/src/gpu/recomp/emit.rs` | no |
| `src/gpu/recomp/mesh_wave.rs` | `crates/xcl/src/gpu/recomp/mesh_wave.rs` | no |
| `src/gpu/recomp/mesh_dispatch.rs` | `crates/xcl/src/gpu/recomp/mesh_dispatch.rs` | no |
| `src/shaders.rs` | `crates/xcl/src/shaders.rs` | yes |
| `src/rpf.rs` | `crates/xcl/src/rpf.rs` | yes |
| `src/rpf_probe.rs` | `crates/xcl/src/rpf_probe.rs` | no |
| `src/util.rs` | `crates/xcl/src/util.rs` | yes |

Not vendored: `gpu/recomp/dxc.rs` (the Windows `dxcompiler.dll` COM interface),
`gpu/recomp/runner.rs` (the D3D12 GPU runner), `gpu/recomp/pso.rs` (the D3D12 PSO
creation), `gpu/recomp/tests.rs` and the other Windows-only modules of XCL. The Linux
counterpart of `gpu/recomp/dxc.rs` is `tools/xcl-recomp/src/dxc.rs` (the `dxc` CLI).

## How it is used

`tools/xcl-recomp` loads the files with `#[path]` (see its `src/gpu.rs`), so the module
paths are the ones of XCL (`crate::gpu::rdna2`, `crate::gpu::recomp`, …) and the
vendored files compile unchanged. The crate also exports a C ABI
(`xcl_translate` / `xcl_free`, see `src/runtime/modules/xcl_recomp.h`).

## License

XCL is GPL-2.0-only; XValkyrie is GPL-2.0-only too. The vendored files keep their
original headers.
