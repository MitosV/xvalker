//! The C ABI the runtime loads (`xcl_tiling.dll`, header `src/runtime/modules/xcl_tiling.h`):
//! the console layout of a texture from its D3D12 description (as `hle::xg` computes it,
//! checked against the console's own library) and the detiling of console memory into linear
//! rows per subresource. All functions return 0 (S_OK) or an HRESULT; handles are opaque.
//!
//! Only 2D textures (arrays, cubes) with one sample and a texel plane: what `xg.rs` models.
//! The caller guarantees `mem` is readable for `mem_bytes`; bytes of a slice past `mem_bytes`
//! read as zero (a streaming pool has only some of its pages mapped).

use crate::gpu::texture::{CONSOLE_PIPES, SERIES_S_PIPES};
use crate::hle::xg;

const E_POINTER: i32 = 0x8000_4003u32 as i32;
const E_INVALIDARG: i32 = 0x8007_0057u32 as i32;

/// The console layout of one texture.
pub struct XclTiling {
    tex: xg::Texture,
}

/// ABI version of this DLL (the runtime checks it before using the rest).
#[no_mangle]
pub extern "C" fn xcl_tiling_version() -> u32 {
    1
}

/// `desc`: the D3D12_RESOURCE_DESC as the console has it (0x38 bytes, the title's own, with
/// its Layout field: 1 linear, 0x100|mode a swizzle mode, 0/2 the library's choice).
/// `series_s`: 0 = Series X pipes / packers, 1 = Series S (only the XOR modes depend on it).
#[no_mangle]
pub unsafe extern "C" fn xcl_tiling_create(desc: *const u8, series_s: u32,
                                           out: *mut *mut XclTiling) -> i32 {
    if desc.is_null() || out.is_null() {
        return E_POINTER;
    }
    *out = std::ptr::null_mut();
    let d = &*(desc as *const [u8; 0x38]);
    let pipes = if series_s != 0 { SERIES_S_PIPES } else { CONSOLE_PIPES };
    match xg::Texture::with_pipes(d, pipes) {
        Ok(tex) => {
            // no texel plane (a depth buffer in a color format, a BCn render target's texels):
            // nothing to detile
            if tex.texels().is_none() {
                return E_INVALIDARG;
            }
            *out = Box::into_raw(Box::new(XclTiling { tex }));
            0
        }
        Err(hr) => hr,
    }
}

#[no_mangle]
pub unsafe extern "C" fn xcl_tiling_destroy(p: *mut XclTiling) {
    if !p.is_null() {
        drop(Box::from_raw(p));
    }
}

/// Bytes the texture takes in the title's memory (its planes, mips and the mip tail).
#[no_mangle]
pub unsafe extern "C" fn xcl_tiling_size(p: *const XclTiling) -> u64 {
    if p.is_null() {
        return 0;
    }
    (*p).tex.size()
}

/// The texel subresources in D3D12 order (mip + slice × mips): array slices × mip levels.
#[no_mangle]
pub unsafe extern "C" fn xcl_tiling_subresources(p: *const XclTiling) -> u32 {
    if p.is_null() {
        return 0;
    }
    let Some(s) = (*p).tex.texels() else { return 0 };
    s.array * s.chain.levels.len() as u32
}

/// One subresource's linear shape: `rows` rows of `row_bytes` (tightly packed, no padding).
#[no_mangle]
pub unsafe extern "C" fn xcl_tiling_subresource(p: *const XclTiling, index: u32,
                                                rows: *mut u32, row_bytes: *mut u32) -> i32 {
    if p.is_null() || rows.is_null() || row_bytes.is_null() {
        return E_POINTER;
    }
    let Some(s) = (*p).tex.texels() else { return E_INVALIDARG };
    let mips = s.chain.levels.len();
    let (i, slice) = (index as usize % mips, index as usize / mips);
    if slice >= s.array as usize {
        return E_INVALIDARG;
    }
    let l = &s.chain.levels[i];
    *rows = l.height as u32;
    *row_bytes = (l.width * s.bpe as usize) as u32;
    0
}

/// Detile subresource `index` of the console memory `[mem, mem + mem_bytes)` into `out`
/// (`rows × row_bytes` of `xcl_tiling_subresource`, tightly packed linear rows).
#[no_mangle]
pub unsafe extern "C" fn xcl_tiling_detile(p: *const XclTiling, mem: *const u8, mem_bytes: u64,
                                           index: u32, out: *mut u8, out_bytes: u64) -> i32 {
    if p.is_null() || out.is_null() {
        return E_POINTER;
    }
    let Some(s) = (*p).tex.texels() else { return E_INVALIDARG };
    let mips = s.chain.levels.len();
    let (i, slice) = (index as usize % mips, index as usize / mips);
    if slice >= s.array as usize {
        return E_INVALIDARG;
    }
    let l = &s.chain.levels[i];
    let need = l.width * l.height * s.bpe as usize;
    if out_bytes < need as u64 {
        return E_INVALIDARG;
    }
    let start = slice * s.chain.slice_bytes;
    let avail = (mem_bytes as usize).saturating_sub(start).min(s.chain.slice_bytes);
    let empty: &[u8] = &[];
    let src = if avail == 0 { empty } else { std::slice::from_raw_parts(mem.add(start), avail) };
    let linear = s.chain.detile_level(src, i, slice);
    std::ptr::copy_nonoverlapping(linear.as_ptr(), out, need);
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gpu::tiling::{Chain, Layout};

    /// A D3D12_RESOURCE_DESC as the console has it (the fields xg.rs reads).
    fn desc(width: u64, height: u32, array: u16, mips: u16, format: u32, layout: u32,
            flags: u32) -> [u8; 0x38] {
        let mut d = [0u8; 0x38];
        d[0..4].copy_from_slice(&3u32.to_le_bytes()); // TEXTURE2D
        d[0x10..0x18].copy_from_slice(&width.to_le_bytes());
        d[0x18..0x1C].copy_from_slice(&height.to_le_bytes());
        d[0x1C..0x1E].copy_from_slice(&array.to_le_bytes());
        d[0x1E..0x20].copy_from_slice(&mips.to_le_bytes());
        d[0x20..0x24].copy_from_slice(&format.to_le_bytes());
        d[0x24..0x28].copy_from_slice(&1u32.to_le_bytes()); // one sample
        d[0x2C..0x30].copy_from_slice(&layout.to_le_bytes());
        d[0x30..0x34].copy_from_slice(&flags.to_le_bytes());
        d
    }

    unsafe fn handle(d: &[u8; 0x38], series_s: u32) -> *mut XclTiling {
        let mut p: *mut XclTiling = std::ptr::null_mut();
        assert_eq!(xcl_tiling_create(d.as_ptr(), series_s, &mut p), 0);
        assert!(!p.is_null());
        p
    }

    /// The layout the console's library answers (xg.rs's own test): RGBA8 256×256, full
    /// chain, 64KB_S — and the detiling of every level through the C ABI.
    #[test]
    fn abi_layout_and_detile_round_trip() {
        unsafe {
            let d = desc(256, 256, 1, 0, 28, 0x109, 0);
            let p = handle(&d, 0);
            assert_eq!(xcl_tiling_size(p), 0x60000);
            assert_eq!(xcl_tiling_subresources(p), 9);
            // tile random rows per level as the console keeps them, detile through the ABI
            let s = (*p).tex.texels().unwrap();
            let chain = &s.chain;
            let mut mem = vec![0u8; xcl_tiling_size(p) as usize];
            let mut n = 0x1234_5678u32;
            let mut want = Vec::new();
            for (i, l) in chain.levels.iter().enumerate() {
                let e = s.bpe as usize;
                let lin: Vec<u8> = (0..l.width * l.height * e)
                    .map(|_| {
                        n = n.wrapping_mul(1_103_515_245).wrapping_add(12345);
                        (n >> 16) as u8
                    })
                    .collect();
                chain.tile_level(&mut mem, i, 0, &lin);
                want.push(lin);
            }
            for (i, lin) in want.iter().enumerate() {
                let (mut rows, mut row_bytes) = (0u32, 0u32);
                assert_eq!(xcl_tiling_subresource(p, i as u32, &mut rows, &mut row_bytes), 0);
                assert_eq!((rows as usize, row_bytes as usize), (chain.levels[i].height, chain.levels[i].width * s.bpe as usize));
                let mut out = vec![0u8; lin.len()];
                assert_eq!(xcl_tiling_detile(p, mem.as_ptr(), mem.len() as u64, i as u32,
                                             out.as_mut_ptr(), out.len() as u64), 0);
                assert_eq!(&out, lin, "level {i}");
            }
            // a short buffer reads as zero past its end, and is refused when too small
            let mut out = vec![0u8; want[0].len()];
            assert_eq!(xcl_tiling_detile(p, mem.as_ptr(), 0, 0, out.as_mut_ptr(), out.len() as u64), 0);
            assert!(out.iter().all(|&b| b == 0));
            assert_eq!(xcl_tiling_detile(p, mem.as_ptr(), mem.len() as u64, 0, out.as_mut_ptr(), 3), E_INVALIDARG);
            assert_eq!(xcl_tiling_subresources(std::ptr::null()), 0);
            xcl_tiling_destroy(p);
        }
    }

    /// A mipped array in the mode the library picks for a plain texture (4KB_S), a BC7 cube
    /// in 64KB_D, and a linear one: every subresource detiles back to what was tiled.
    #[test]
    fn abi_arrays_cubes_bc_and_linear() {
        unsafe {
            for (d, pipes) in [
                (desc(200, 72, 3, 8, 28, 0, 0), CONSOLE_PIPES),          // RGBA8 array, optimal
                (desc(256, 256, 6, 9, 98, 0x10A, 0), CONSOLE_PIPES),     // BC7 cube, 64KB_D
                (desc(100, 30, 1, 7, 28, 1, 0), CONSOLE_PIPES),          // linear
                (desc(96, 40, 2, 6, 28, 0x115, 0), SERIES_S_PIPES),      // 4KB_S_X, Series S
            ] {
                let series_s = if pipes == SERIES_S_PIPES { 1 } else { 0 };
                let p = handle(&d, series_s);
                let s = (*p).tex.texels().unwrap();
                let chain = &s.chain;
                let size = xcl_tiling_size(p) as usize;
                assert_eq!(size, chain.slice_bytes * s.array as usize);
                let mut mem = vec![0u8; size];
                let mut n = 7u32;
                let mut want = Vec::new();
                for slice in 0..s.array as usize {
                    let at = slice * chain.slice_bytes;
                    for (i, l) in chain.levels.iter().enumerate() {
                        let e = s.bpe as usize;
                        let lin: Vec<u8> = (0..l.width * l.height * e)
                            .map(|_| {
                                n ^= n << 13;
                                n ^= n >> 17;
                                n ^= n << 5;
                                n as u8
                            })
                            .collect();
                        chain.tile_level(&mut mem[at..at + chain.slice_bytes], i, slice, &lin);
                        want.push(lin);
                    }
                }
                assert_eq!(xcl_tiling_subresources(p) as usize, want.len());
                for (k, lin) in want.iter().enumerate() {
                    let mut out = vec![0u8; lin.len()];
                    assert_eq!(xcl_tiling_detile(p, mem.as_ptr(), size as u64, k as u32,
                                                 out.as_mut_ptr(), out.len() as u64), 0);
                    assert_eq!(&out, lin, "subresource {k}");
                }
                xcl_tiling_destroy(p);
            }
        }
    }

    /// What the library does not model is refused, not guessed: 3D, MSAA, a depth buffer in
    /// a color format (no texel plane), an unknown format.
    #[test]
    fn abi_refuses_what_xg_does_not_model() {
        unsafe {
            let mut p: *mut XclTiling = std::ptr::null_mut();
            let mut d3 = desc(64, 64, 8, 1, 28, 0, 0);
            d3[0..4].copy_from_slice(&4u32.to_le_bytes()); // TEXTURE3D
            assert_ne!(xcl_tiling_create(d3.as_ptr(), 0, &mut p), 0);
            let mut msaa = desc(64, 64, 1, 1, 28, 0, 0);
            msaa[0x24..0x28].copy_from_slice(&2u32.to_le_bytes());
            assert_ne!(xcl_tiling_create(msaa.as_ptr(), 0, &mut p), 0);
            // a depth buffer in a color format: the library leaves its texels out
            let ds = desc(64, 64, 1, 1, 28, 0, 2);
            assert_ne!(xcl_tiling_create(ds.as_ptr(), 0, &mut p), 0);
            let unknown = desc(64, 64, 1, 1, 200, 0, 0);
            assert_ne!(xcl_tiling_create(unknown.as_ptr(), 0, &mut p), 0);
            assert_eq!(xcl_tiling_create(std::ptr::null(), 0, &mut p), E_POINTER);
        }
    }

    /// The pipe configuration only changes the XOR modes: 64KB_S (no XOR) is the same on
    /// both consoles, 4KB_S_X is not.
    #[test]
    fn abi_series_pipes_only_change_xor_modes() {
        unsafe {
            let plain = desc(128, 128, 1, 1, 28, 0x109, 0);
            let a = handle(&plain, 0);
            let b = handle(&plain, 1);
            assert_eq!(xcl_tiling_size(a), xcl_tiling_size(b));
            let xor = desc(128, 128, 1, 1, 28, 0x115, 0);
            let x = handle(&xor, 0);
            let y = handle(&xor, 1);
            let sx = (*x).tex.texels().unwrap();
            let sy = (*y).tex.texels().unwrap();
            let cx = Chain::new(Layout::new(21, 2, CONSOLE_PIPES).unwrap(), 128, 128, 1, 1);
            let cy = Chain::new(Layout::new(21, 2, SERIES_S_PIPES).unwrap(), 128, 128, 1, 1);
            assert_eq!(sx.chain.slice_bytes, cx.slice_bytes);
            assert_eq!(sy.chain.slice_bytes, cy.slice_bytes);
            let mut mem = vec![0u8; cx.slice_bytes];
            let lin: Vec<u8> = (0..128 * 128 * 4).map(|i| (i * 7 + i / 977) as u8).collect();
            cx.tile_level(&mut mem, 0, 0, &lin);
            let mut out = vec![0u8; lin.len()];
            assert_eq!(xcl_tiling_detile(x, mem.as_ptr(), mem.len() as u64, 0,
                                         out.as_mut_ptr(), out.len() as u64), 0);
            assert_eq!(out, lin);
            // the same memory read with the other console's pipes is not the same picture
            assert_eq!(xcl_tiling_detile(y, mem.as_ptr(), mem.len() as u64, 0,
                                         out.as_mut_ptr(), out.len() as u64), 0);
            assert_ne!(out, lin);
            for p in [a, b, x, y] {
                xcl_tiling_destroy(p);
            }
        }
    }
}