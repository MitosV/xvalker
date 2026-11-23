//! The vendored XCL GPU modules, wired together exactly as in XCL's own crate.
//!
//! XCL keeps these under `crates/xcl/src/gpu/`. Here they are loaded from
//! `third_party/xcl/src/gpu/` (see `third_party/xcl/README.md`), so the recompiler
//! can be built and audited outside XCL.

#[path = "../../../third_party/xcl/src/gpu/dxbc.rs"]
pub mod dxbc;
#[path = "../../../third_party/xcl/src/gpu/pack.rs"]
pub mod pack;
#[path = "../../../third_party/xcl/src/gpu/rdna2.rs"]
pub mod rdna2;
#[path = "../../../third_party/xcl/src/gpu/rdna2_spec.rs"]
pub mod rdna2_spec;
#[path = "../../../third_party/xcl/src/gpu/rdna2_xbox.rs"]
pub mod rdna2_xbox;
#[path = "../../../third_party/xcl/src/gpu/shader.rs"]
pub mod shader;
#[path = "../../../third_party/xcl/src/gpu/xbox.rs"]
pub mod xbox;

/// The recompiler proper (`gpu::recomp` in XCL). Its own children (`emit`,
/// `mesh_dispatch`, `mesh_wave`) are the files that sit next to it in
/// `third_party/xcl/src/gpu/recomp/`.
#[path = "../../../third_party/xcl/src/gpu/recomp/mod.rs"]
pub mod recomp;
