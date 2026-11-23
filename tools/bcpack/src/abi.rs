//! The `crate::abi` module the vendored BCPack sources expect.
//!
//! Only the console's `dstorage_xs.h` values used by the wrapper and by the
//! vendored tests live here; the real console ABI is in
//! `src/runtime/modules/dstorage.cpp` and in XCL's `hle/dstorage.rs`.
#![allow(dead_code)]

pub const S_OK: i32 = 0;
pub const E_POINTER: i32 = 0x8000_4003u32 as i32;
pub const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;

pub const E_DSTORAGE_ACCESS_VIOLATION: i32 = 0x8924_0009u32 as i32;
pub const E_DSTORAGE_INVALID_DESTINATION_SIZE: i32 = 0x8924_000fu32 as i32;
pub const E_DSTORAGE_DECOMPRESSION_ERROR: i32 = 0x8924_0030u32 as i32;
pub const E_DSTORAGE_ZLIB_BAD_DATA: i32 = 0x8924_0032u32 as i32;

pub const OPT_ZLIB: u64 = 1;
pub const OPT_BCPACK_SHIFT: u32 = 1;
pub const OPT_BCPACK_MASK: u64 = 0xf;
