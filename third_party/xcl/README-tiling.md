# XCL, vendored: the console's texture layout (GFX10 tiling)

Source: https://github.com/gefydev/xcl (the author allows copying; same author as this
project), commit `dd713e3`, repository checkout at `~/dev/projects/xcl` (read-only).
Vendored byte-identical, unmodified, under `src/` with the paths they have there
(`crates/xcl/src/…` → `src/…`):

| File here | There | What it is |
| --- | --- | --- |
| `src/hle/d3d12x/title_textures.rs` | `crates/xcl/src/hle/d3d12x/title_textures.rs` | the design: a PC texture kept in step with the title's memory (write-watch), uploads batched before the title's next ExecuteCommandLists |
| `src/hle/xg.rs` | `crates/xcl/src/hle/xg.rs` | the console's layout library: how a D3D12 texture the title owns lies in its memory (planes, mips, the mip tail), checked against the console's own `xg_xs.dll` |
| `src/gpu/tiling.rs` | `crates/xcl/src/gpu/tiling.rs` | GFX10 (RDNA2) tiling from AMD addrlib swizzle patterns: `Layout`, `Chain` (mips, slices, the mip tail), `detile`/`tile` |
| `src/gpu/tiling_tables.rs` | `crates/xcl/src/gpu/tiling_tables.rs` | the swizzle patterns (generated from AMD addrlib, MIT) |
| `src/gpu/texture.rs` | `crates/xcl/src/gpu/texture.rs` | console textures → PC textures: `placement`, `subresources`, `console_pipes`, the D3D12 upload paths |
| `src/gpu/desc.rs` | `crates/xcl/src/gpu/desc.rs` | GFX10 descriptors (T#) and their D3D12 terms |

These files are the reference of what the runtime does. The parts the runtime builds live in
`tools/xcl-tiling` (a C ABI over the layout and detiling, `xcl_tiling.dll`) and in
`src/runtime/modules/d3d12x/title_textures.cpp` (the design of `title_textures.rs` in the
runtime's D3D12X). Every line where those differ from the files here is recorded in
`PATCHES-tiling.md`.

Licenses: XCL's own files carry its license (see XCL's repository); `tiling_tables.rs` is
generated from AMD addrlib (Mesa, `src/amd/addrlib/src/gfx10/gfx10SwizzlePattern.h`),
Copyright (C) 2007-2026 Advanced Micro Devices, Inc., MIT.

This file is about the tiling files only. `README.md` is written by another track.
