//! XCL's GPU module, vendored: tiling and its tables are included byte-identical from
//! third_party/xcl; desc and texture are the copies of this crate (see PATCHES-tiling.md).

#[path = "../../../../third_party/xcl/src/gpu/tiling_tables.rs"]
pub mod tiling_tables;
#[path = "../../../../third_party/xcl/src/gpu/tiling.rs"]
pub mod tiling;
pub mod desc;
pub mod texture;