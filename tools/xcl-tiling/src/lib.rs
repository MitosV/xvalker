//! xcl-tiling: the console's texture layout and detiling, behind a C ABI (`xcl_tiling.dll`).
//!
//! The layout and detiling are XCL's, vendored byte-identical under `third_party/xcl/src/`
//! (commit dd713e3): `gpu::tiling` (+ its tables) and `hle::xg` are included from there;
//! `gpu::desc` and `gpu::texture` are copies whose `windows` terms are the plain data of
//! `d3d12` (generated from the SDK) and whose GPU upload paths are left out — the runtime
//! does those itself. Every difference from the vendored files is in
//! `third_party/xcl/PATCHES-tiling.md`.
//!
//! `src/abi.rs` is the C ABI the runtime loads: the console layout of a D3D12 description
//! (as `hle::xg` computes it) and the detiling of console memory into linear rows per
//! subresource. `hle::d3d12x::title_textures.rs` (vendored, not built here) is the design
//! the runtime's `title_textures.cpp` follows.

// the vendored files cast function items to usize for their export tables (XCL's own way)
#![allow(function_casts_as_integer)]

/// Build an export list from functions whose Rust names equal the exported names (XCL's own).
#[macro_export]
macro_rules! hle_exports {
    ($($name:ident),* $(,)?) => {
        vec![$( (stringify!($name), $name as usize) ),*]
    };
}

/// XCL's log macros, to stderr here.
#[macro_export]
macro_rules! xlog_warn {
    ($($arg:tt)*) => { eprintln!($($arg)*) };
}
#[macro_export]
macro_rules! xlog_info {
    ($($arg:tt)*) => { eprintln!($($arg)*) };
}

/// XCL's `util`: the HRESULT terms the vendored files use.
pub mod util {
    pub type HRESULT = i32;
    pub const S_OK: HRESULT = 0;
}

/// XCL's `state::config()`: which console the title runs as. The C ABI takes the pipe
/// configuration per call (`series_s`); this default is for `Texture::new`'s own path.
pub mod state {
    use std::sync::atomic::{AtomicBool, Ordering};
    static SERIES_S: AtomicBool = AtomicBool::new(false);
    pub struct Config;
    impl Config {
        pub fn is_series_s(&self) -> bool {
            SERIES_S.load(Ordering::Relaxed)
        }
    }
    pub fn config() -> Config {
        Config
    }
    pub fn set_series_s(on: bool) {
        SERIES_S.store(on, Ordering::Relaxed);
    }
}

/// XCL's `stubs`: the vendored `xg.rs` fills the computer's last vtable slots with stubs.
/// Here they all answer E_NOTIMPL; nothing calls them through this crate.
pub mod stubs {
    pub struct StubInfo;
    unsafe extern "system" fn not_implemented() -> u32 {
        0x8000_4001
    }
    pub fn make_stub_info(_dll: &str, _name: &str, _ret: u64) -> (usize, &'static StubInfo) {
        static INFO: StubInfo = StubInfo;
        (not_implemented as usize, &INFO)
    }
}

pub mod d3d12;

pub mod gpu;
pub mod hle;

mod abi;