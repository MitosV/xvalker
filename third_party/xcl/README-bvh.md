# third_party/xcl/src/gpu — the console's acceleration structures

`src/gpu/bvh.rs` is **XCL**'s module for the acceleration structures a console title builds in
the RDNA2 node format (the ones `image_bvh(64)_intersect_ray` traverses). It is vendored
**byte for byte**: the file is the same as in XCL, with not a character changed.

| | |
|---|---|
| Source | `~/dev/projects/xcl`, `crates/xcl/src/gpu/bvh.rs` |
| Commit | `db5dc26` ("Shader loading: dumps on request, translations kept on disk, precompile of compute") |
| SHA-256 | `9efa6e2afe1b26b5a35cca68663dd1c5f52eecb985e39c0af0f3392f42076e7c` |

The edits to it, if any, are listed in `PATCHES-bvh.md` (there are none).

## What it holds

* the format: a 256-byte header (the root node id at 0x30), 64-byte aligned nodes whose id is
  `(offset / 8) | kind`, the kinds 0-3 (a pair of triangles), 4 (a box of half floats), 5 (a box
  of floats) and 6 (an instance), and what each of them means;
* the builder: a bottom level over triangles (`build`) and a top level over instances
  (`build_top`), top-down, by the median split of the leaf box centroids;
* the CPU reference: `intersect_node` (one node), `Bvh::trace` (a bottom level) and
  `Bvh::trace_scene` (through a top level), with the same results as the shaders' traversal;
* what the console's own builder says about sizes: `prebuild_size`,
  `is_compressed_header`.

## How it is used here

The crate `tools/xcl-bvh` includes the file with `#[path]` (no copy of its own) and gives it a C
interface, which is what the runtime has: `src/runtime/modules/xcl_bvh.h`
(`xcl_bvh_validate`, `xcl_bvh_trace`, `xcl_bvh_trace_scene`). The tool
`tools/xcl-bvh/src/bin/xcl_bvh_reference.rs` builds a bottom level and a top level over three of
them, reads both through that interface, and checks every ray against a brute-force search over
the same triangles.
