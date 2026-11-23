# Patches to the vendored XCL files

`src/gpu/bvh.rs` is vendored **byte for byte**: no line of it was changed, so there is nothing to
apply and nothing to keep in step.

The SHA-256 of the file as it is here and as it is in XCL (`db5dc26`,
`crates/xcl/src/gpu/bvh.rs`):

```
9efa6e2afe1b26b5a35cca68663dd1c5f52eecb985e39c0af0f3392f42076e7c
```

Check it with:

```sh
sha256sum third_party/xcl/src/gpu/bvh.rs
```

What the crate `tools/xcl-bvh` adds around it (its own file, `tools/xcl-bvh/src/lib.rs`):

* the C interface (`xcl_bvh_validate`, `xcl_bvh_trace`, `xcl_bvh_trace_scene`), which is what
  `src/runtime/modules/xcl_bvh.h` declares;
* the walk over the nodes, the count of each kind and the geometry a hit is in (the vendored
  module returns the triangle inside its geometry, not the geometry itself);
* `#![allow(dead_code)]` in the crate, because the vendored module has more than the interface
  uses.
