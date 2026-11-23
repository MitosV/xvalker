//! `xg_xs.dll` – the console's texture layout library: where the mips and texels of a texture
//! go in the title's own memory (Fortnite imports `XGCreateTextureComputer`). XCL answers from
//! its own GFX10 model (`gpu::tiling::Chain`, checked on retail textures) in the console's
//! structures; `tests/xg_oracle.rs` compares the answers with the console's own library
//! (`XCL_SAMPLE_XG`).
//!
//! * `XGCreateTextureComputer(D3D12_RESOURCE_DESC*, computer**)`: 2D textures, arrays and
//!   cubes with one sample. `Layout` 1 = linear, 0x100–0x11F = swizzle mode `Layout & 0x1F`,
//!   0 / 2 = the library's choice (`optimal_mode`).
//! * The computer (no QueryInterface): AddRef, Release, GetResourceLayout(XG_RESOURCE_LAYOUT*),
//!   GetResourceSizeBytes, GetResourceBaseAlignmentBytes, GetMipLevelOffsetBytes(plane, mip),
//!   GetTexelElementOffsetBytes(plane, mip, x, y, slice, sample, bit*), GetTexelCoordinate,
//!   CopyIntoSubresource(dst, plane, subresource, src, row pitch, slice pitch),
//!   CopyFromSubresource(dst, plane, subresource, src, row pitch, slice pitch), then 2 more.
//! * XG_RESOURCE_LAYOUT (0x1B40): size @0, alignment @8, mips @0x10, planes @0x14, planes
//!   from 0x18, 0x6C8 bytes each (usage @0, size @8, slice size @0x10, alignment @0x20, bytes
//!   per element @0x28, first mip in the tail @0x2C, mips in the tail @0x30, then 15 mips of
//!   0x70 from 0x38: size @0, offset @8, slice size @0x10, pitch in pixels @0x18 / bytes
//!   @0x1C, alignment @0x20, padded width / height / depth @0x24 / 0x28 / 0x2C, width /
//!   height in elements @0x30 / 0x34 and pixels @0x38 / 0x3C, depth @0x40, samples @0x44,
//!   swizzle mode @0x48, 1 @0x60, in the tail @0x64), dimension @0x1B38. Levels in the mip
//!   tail are reported as 256-byte tiles at their offset inside the tail block.

#![allow(non_snake_case)]

use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::OnceLock;

use crate::gpu::tiling::{Chain, Layout};
use crate::util::{HRESULT, S_OK};

const E_NOTIMPL: HRESULT = 0x8000_4001u32 as i32;
const E_POINTER: HRESULT = 0x8000_4003u32 as i32;
const E_INVALIDARG: HRESULT = 0x8007_0057u32 as i32;

pub const LAYOUT_BYTES: usize = 0x1B40;
const PLANE: usize = 0x18;
const MIP: usize = 0x38;
const MIP_BYTES: usize = 0x70;
const PLANE_BYTES: usize = MIP + 15 * MIP_BYTES;
const MAX_MIPS: usize = 15;

pub fn exports() -> Vec<(&'static str, usize)> {
    crate::hle_exports![XGCreateTextureComputer]
}

/// The swizzle mode the console's library picks for a 2D texture with `Layout` 0 / 2: 64KB_D
/// for `Layout` 2, depth 64KB_Z_X, render targets 64KB_R_X, otherwise 4KB_S – or for UAVs
/// 4KB_S_X up to 64 KB of texels, else 64KB_R_X.
pub fn optimal_mode(layout: u32, flags: u32, bits_per_pixel: u64, width: u64, height: u32) -> u32 {
    let (rt, depth, uav) = (flags & 1 != 0, flags & 2 != 0, flags & 4 != 0);
    if depth {
        24
    } else if layout == 2 {
        10
    } else if rt {
        27
    } else if !uav {
        5
    } else if bits_per_pixel * width * height as u64 <= 0x8_0000 {
        21
    } else {
        27
    }
}

/// One tiled surface of a texture (color, depth or stencil) as the console lays it out.
pub struct Surface {
    pub chain: Chain,
    pub mode: u32,
    pub bpe: u32,
    /// pixels per element side (4 for BCn)
    pub side: u32,
    pub width: u32,
    pub height: u32,
    pub array: u32,
    pub first_tail: usize,
}

/// What a plane of a texture holds.
pub enum PlaneKind {
    Surface(Surface),
    /// compression metadata (CMASK of a render target, HTILE of a depth buffer): only its size
    Meta { bytes: u64, align: u64 },
    /// a plane the library leaves empty (the texels of a BCn render target, the depth of a
    /// depth buffer in a color format)
    Blank,
}

/// A plane of a texture: its usage code in XG_RESOURCE_LAYOUT (1 color, 2 CMASK, 4 HTILE,
/// 5 / 6 depth / stencil, 7 / 8 depth / stencil of a depth buffer) and offset in the resource.
pub struct Plane {
    pub usage: u32,
    pub offset: u64,
    pub kind: PlaneKind,
}

/// A 2D texture as the console lays it out: its planes, and the one the texels are in.
pub struct Texture {
    pub planes: Vec<Plane>,
    pub main: usize,
    pub mips: u32,
    /// a depth buffer in a color format: the library reports a size of 0
    pub empty: bool,
}

/// (log2 bytes per element, pixels per element side) of a DXGI format; None for the 96-bit
/// and packed 4:2:2 ones (not tiled) and unknown formats
pub fn element(f: u32) -> Option<(u32, u32)> {
    Some(match f {
        1..=4 => (4, 1),
        9..=22 => (3, 1),
        23..=47 | 67 | 87..=93 => (2, 1),
        48..=59 | 85 | 86 | 115 => (1, 1),
        60..=65 => (0, 1),
        70..=72 | 79..=81 => (3, 4),
        73..=78 | 82..=84 | 94..=99 => (4, 4),
        _ => return None,
    })
}

/// 256-byte tile size in elements (log2 bytes per element → (w, h))
fn micro(e: u32) -> (usize, usize) {
    (1 << [4, 4, 3, 3, 2][e as usize], 1 << [4, 3, 3, 2, 2][e as usize])
}

impl Texture {
    /// From a D3D12_RESOURCE_DESC (0x38 bytes) as the console's.
    pub fn new(d: &[u8; 0x38]) -> Result<Texture, HRESULT> {
        Self::with_pipes(d, crate::gpu::texture::console_pipes())
    }

    /// `new` for a given pipe / packer configuration (only the XOR modes depend on it).
    pub fn with_pipes(d: &[u8; 0x38], pipes: crate::gpu::tiling::PipeConfig) -> Result<Texture, HRESULT> {
        let u = |o: usize| u32::from_le_bytes(d[o..o + 4].try_into().unwrap());
        let w16 = |o: usize| u16::from_le_bytes(d[o..o + 2].try_into().unwrap()) as u32;
        let (dim, width, height, array, mips, format, samples, layout, flags) =
            (u(0), u64::from_le_bytes(d[0x10..0x18].try_into().unwrap()), u(0x18), w16(0x1C), w16(0x1E), u(0x20), u(0x24), u(0x2C), u(0x30));
        // 1D / 3D / buffers and MSAA are not modelled
        if dim != 3 || samples > 1 {
            return Err(E_NOTIMPL);
        }
        if width == 0 || width > 16384 || height == 0 || height > 16384 || array == 0 {
            return Err(E_INVALIDARG);
        }
        // depth + stencil formats: a 4-byte depth plane and a 1-byte stencil plane
        let planar = matches!(format, 19..=22 | 44..=47);
        let (e, side) = if planar { (2, 1) } else { element(format).ok_or(E_INVALIDARG)? };
        let mode = match layout {
            1 => 0,
            0x100..=0x11F => layout & 0x1F,
            // the choice goes by the whole format's size (D32_S8X24: 64 bits a pixel)
            0 | 2 => optimal_mode(layout, flags, if matches!(format, 19..=22) { 64 } else { (8u64 << e) / (side * side) as u64 }, width, height),
            _ => return Err(E_INVALIDARG),
        };
        let full = 32 - (width as u32).max(height).leading_zeros();
        let mips = if mips == 0 { full } else { mips };
        if mips as usize > MAX_MIPS {
            return Err(E_INVALIDARG);
        }
        let surface = |e: u32, side: u32| Surface::new(mode, e, side, width as u32, height, array, mips, pipes);
        let (rt, ds) = (flags & 1 != 0, flags & 2 != 0);
        if planar && layout == 2 {
            return Err(E_INVALIDARG); // a stencil format with the 64KB_D choice
        }
        // a depth buffer (one level: it gets an HTILE) is in the Z modes; in a color format it
        // is left empty. Render target + depth flags: a render target in a Z mode, unless the
        // format has a stencil
        let depth_buffer = ds && mips == 1 && (planar || !rt);
        if depth_buffer && mode != 24 {
            return Err(E_INVALIDARG);
        }
        let depth_format = planar || matches!(format, 39 | 40 | 53 | 55);
        // the texels of a BCn render target / depth buffer of one level are left out too
        let blank = (depth_buffer && !depth_format) || ((rt || ds) && side > 1 && mips == 1);
        // and then the resource is empty – except a render target's CMASK
        let empty = blank && ds;
        let main = if blank { PlaneKind::Blank } else { PlaneKind::Surface(surface(e, side)?) };
        let stencil = if planar { Some(surface(0, 1)?) } else { None };
        // metadata planes, per slice: CMASK 1 byte per 128 pixels, HTILE per 16, of the size
        // padded to 1024 × 1024
        let meta = |div: u64| (width.div_ceil(1024) * 1024) * (height.div_ceil(1024) * 1024) as u64 / div * array as u64;
        let mut planes: Vec<(u32, PlaneKind)> = Vec::new();
        let (main_usage, stencil_usage) = if ds { (7, 8) } else if planar { (5, 6) } else { (1, 0) };
        if depth_buffer {
            planes.push((4, PlaneKind::Meta { bytes: meta(16), align: 0x1_0000 }));
        }
        let main_at = planes.len();
        planes.push((if ds && !planar && !depth_buffer { 1 } else { main_usage }, main));
        if let Some(s) = stencil {
            planes.push((stencil_usage, PlaneKind::Surface(s)));
        }
        // a render target of one level gets a CMASK (not with the 64KB_D choice or the _T modes)
        if rt && !depth_buffer && !planar && !empty && mips == 1 && layout != 2 && !(0x110..=0x113).contains(&layout) {
            planes.push((2, PlaneKind::Meta { bytes: meta(128), align: 0x2000 }));
        }
        let mut offset = 0u64;
        let planes = planes
            .into_iter()
            .map(|(usage, kind)| {
                let (bytes, align) = match &kind {
                    PlaneKind::Surface(s) => (s.size(), s.alignment()),
                    PlaneKind::Meta { bytes, align } => (*bytes, *align),
                    PlaneKind::Blank => (0, 0),
                };
                // an empty plane is at 0 and takes no room
                let at = if matches!(kind, PlaneKind::Blank) { 0 } else { offset.div_ceil(align) * align };
                offset = offset.max(at + bytes);
                Plane { usage, offset: at, kind }
            })
            .collect();
        Ok(Texture { planes, main: main_at, mips, empty })
    }

    pub fn size(&self) -> u64 {
        if self.empty {
            return 0;
        }
        self.planes.iter().map(|p| p.offset + p.bytes()).max().unwrap_or(0)
    }

    pub fn alignment(&self) -> u64 {
        self.planes.iter().map(|p| p.align()).max().unwrap_or(0x100)
    }

    /// the surface of plane `plane` and where it starts (None: metadata or no such plane)
    pub fn plane_surface(&self, plane: u32) -> Option<(&Surface, u64)> {
        let p = self.planes.get(plane as usize)?;
        match &p.kind {
            PlaneKind::Surface(s) => Some((s, p.offset)),
            PlaneKind::Meta { .. } | PlaneKind::Blank => None,
        }
    }

    /// the surface the texels are in (None: the library leaves them out)
    pub fn texels(&self) -> Option<&Surface> {
        self.plane_surface(self.main as u32).map(|(s, _)| s)
    }

    /// byte offset of level `i` of the texels from the start of the resource's first slice
    pub fn mip_offset(&self, i: usize) -> u64 {
        self.plane_surface(self.main as u32).map_or(0, |(s, at)| at + s.mip_offset(i))
    }

    /// byte offset of element (x, y) of level `i` in slice `slice` of the texels
    pub fn element_offset(&self, i: usize, x: usize, y: usize, slice: usize) -> u64 {
        self.plane_surface(self.main as u32).map_or(0, |(s, at)| at + s.element_offset(i, x, y, slice))
    }

    /// XG_RESOURCE_LAYOUT
    pub fn write(&self, b: &mut [u8; LAYOUT_BYTES]) {
        b.fill(0);
        let q = |b: &mut [u8; LAYOUT_BYTES], o: usize, v: u64| b[o..o + 8].copy_from_slice(&v.to_le_bytes());
        let w = |b: &mut [u8; LAYOUT_BYTES], o: usize, v: u32| b[o..o + 4].copy_from_slice(&v.to_le_bytes());
        q(b, 0, self.size());
        q(b, 8, self.alignment());
        w(b, 0x10, self.mips);
        w(b, 0x14, self.planes.len() as u32);
        w(b, 0x1B38, 3);
        for (k, p) in self.planes.iter().enumerate() {
            let pb = PLANE + k * PLANE_BYTES;
            w(b, pb, p.usage);
            q(b, pb + 0x18, p.offset);
            match &p.kind {
                PlaneKind::Surface(s) => s.write_plane(b, pb),
                PlaneKind::Blank => {}
                PlaneKind::Meta { bytes, align } => {
                    q(b, pb + 8, *bytes);
                    q(b, pb + 0x20, *align);
                    q(b, pb + MIP, *bytes);
                    w(b, pb + MIP + 0x20, *align as u32);
                }
            }
        }
    }
}

impl Plane {
    fn bytes(&self) -> u64 {
        match &self.kind {
            PlaneKind::Surface(s) => s.size(),
            PlaneKind::Meta { bytes, .. } => *bytes,
            PlaneKind::Blank => 0,
        }
    }

    fn align(&self) -> u64 {
        match &self.kind {
            PlaneKind::Surface(s) => s.alignment(),
            PlaneKind::Meta { align, .. } => *align,
            PlaneKind::Blank => 0,
        }
    }
}

impl Surface {
    /// `width` × `height` pixels, `array` slices, `mips` levels of `1 << e`-byte elements in
    /// swizzle mode `mode`.
    #[allow(clippy::too_many_arguments)]
    fn new(mode: u32, e: u32, side: u32, width: u32, height: u32, array: u32, mips: u32, pipes: crate::gpu::tiling::PipeConfig) -> Result<Surface, HRESULT> {
        // the modes the console's library takes for a 2D texture (VAR 28 / 31: not modelled)
        let bc = side > 1;
        match mode {
            0 | 1 | 2 | 5 | 6 | 9 | 10 | 17 | 18 | 21 | 22 | 25 | 26 | 27 => {}
            24 if e <= 3 && !bc => {}
            28 | 31 => return Err(E_NOTIMPL),
            _ => return Err(E_INVALIDARG),
        }
        let l = Layout::new(mode, e, pipes).ok_or(E_INVALIDARG)?;
        let chain = Chain::new(l, width, height, side, mips);
        let first_tail = chain.levels.iter().position(|m| m.tail.is_some()).unwrap_or(chain.levels.len());
        Ok(Surface { chain, mode, bpe: 1 << e, side, width, height, array, first_tail })
    }

    pub fn size(&self) -> u64 {
        (self.chain.slice_bytes * self.array as usize) as u64
    }

    pub fn alignment(&self) -> u64 {
        self.chain.layout.block_bytes as u64
    }

    /// (padded width, padded height) in elements of level `i`
    fn padded(&self, i: usize) -> (usize, usize) {
        let l = &self.chain.layout;
        if i < self.first_tail {
            let h = (self.height.div_ceil(self.side) as usize).div_ceil(1 << i);
            (self.chain.levels[i].pitch, h.div_ceil(l.block_h) * l.block_h)
        } else {
            let (mw, mh) = micro(l.elem_log2);
            let k = i - self.first_tail;
            (((l.block_w / 2) >> k).max(mw), (l.block_h >> k).max(mh))
        }
    }

    /// byte offset of level `i` from the start of a slice
    pub fn mip_offset(&self, i: usize) -> u64 {
        let m = &self.chain.levels[i];
        m.tail.unwrap_or(m.offset) as u64
    }

    /// byte offset of element (x, y) of level `i` in slice `slice`; the slice is z of the 2D
    /// patterns (the XOR modes take pipe bits from it)
    pub fn element_offset(&self, i: usize, x: usize, y: usize, slice: usize) -> u64 {
        let m = &self.chain.levels[i];
        (slice * self.chain.slice_bytes + m.offset + self.chain.layout.offset_z(x + m.origin.0, y + m.origin.1, slice, m.pitch)) as u64
    }

    /// this surface's plane of XG_RESOURCE_LAYOUT (its header at `pb`)
    fn write_plane(&self, b: &mut [u8; LAYOUT_BYTES], pb: usize) {
        let q = |b: &mut [u8; LAYOUT_BYTES], o: usize, v: u64| b[o..o + 8].copy_from_slice(&v.to_le_bytes());
        let w = |b: &mut [u8; LAYOUT_BYTES], o: usize, v: u32| b[o..o + 4].copy_from_slice(&v.to_le_bytes());
        let mips = self.chain.levels.len();
        q(b, pb + 8, self.size());
        q(b, pb + 0x10, self.chain.slice_bytes as u64);
        q(b, pb + 0x20, self.alignment());
        w(b, pb + 0x28, self.bpe);
        w(b, pb + 0x2C, self.first_tail as u32);
        w(b, pb + 0x30, (mips - self.first_tail) as u32);
        for i in 0..mips {
            let m = &self.chain.levels[i];
            let r = pb + MIP + i * MIP_BYTES;
            let tail = i >= self.first_tail;
            let (pw, ph) = self.padded(i);
            let bytes = (pw * ph) as u64 * self.bpe as u64;
            let size = bytes;
            q(b, r, size);
            q(b, r + 8, self.mip_offset(i));
            q(b, r + 0x10, size);
            w(b, r + 0x18, pw as u32 * self.side);
            w(b, r + 0x1C, pw as u32 * self.bpe);
            w(b, r + 0x20, if tail { 0x100 } else { self.alignment() as u32 });
            w(b, r + 0x24, pw as u32);
            w(b, r + 0x28, ph as u32);
            w(b, r + 0x2C, self.array);
            w(b, r + 0x30, m.width as u32);
            w(b, r + 0x34, m.height as u32);
            w(b, r + 0x38, (self.width >> i).max(1));
            w(b, r + 0x3C, (self.height >> i).max(1));
            w(b, r + 0x40, self.array);
            w(b, r + 0x44, 1);
            // the tail is in 256-byte tiles of the same kind (S 1, D 2, R 3; Z keeps its mode)
            w(b, r + 0x48, if tail && !self.mode.is_multiple_of(4) { self.mode % 4 } else { self.mode });
            if !tail && (24..=27).contains(&self.mode) {
                w(b, r + 0x50, 0x8000);
                w(b, r + 0x58, 0x8000);
            }
            w(b, r + 0x60, 1);
            w(b, r + 0x64, tail as u32);
        }
    }
}

// ---------------------------------------------------------------------------------------
// the computer
// ---------------------------------------------------------------------------------------

#[repr(C)]
struct Computer {
    vtbl: *const usize,
    refs: AtomicU32,
    tex: Texture,
}

fn vtbl() -> &'static [usize] {
    static V: OnceLock<&'static [usize]> = OnceLock::new();
    V.get_or_init(|| {
        let mut v = vec![
            add_ref as usize,
            release as usize,
            get_resource_layout as usize,
            get_size as usize,
            get_alignment as usize,
            get_mip_offset as usize,
            get_texel_offset as usize,
            not_implemented as usize,
            copy_into as usize,
            copy_from as usize,
        ];
        for i in v.len()..12 {
            let (stub, _) = crate::stubs::make_stub_info("xg_xs.dll", &format!("XGTextureAddressComputer::#{i}"), E_NOTIMPL as u32 as u64);
            v.push(stub);
        }
        Box::leak(v.into_boxed_slice())
    })
}

unsafe fn tex<'a>(p: *mut c_void) -> &'a Texture {
    &(*(p as *const Computer)).tex
}

/// `HRESULT XGCreateTextureComputer(const D3D12_RESOURCE_DESC*, XGTextureAddressComputer**)`
pub unsafe extern "system" fn XGCreateTextureComputer(desc: *const u8, out: *mut *mut c_void) -> HRESULT {
    if desc.is_null() || out.is_null() {
        return E_POINTER;
    }
    *out = std::ptr::null_mut();
    match Texture::new(&*(desc as *const [u8; 0x38])) {
        Ok(tex) => {
            *out = Box::into_raw(Box::new(Computer { vtbl: vtbl().as_ptr(), refs: AtomicU32::new(1), tex })) as *mut c_void;
            S_OK
        }
        Err(hr) => {
            static N: AtomicU32 = AtomicU32::new(0);
            if N.fetch_add(1, Ordering::Relaxed) < 8 {
                let d = &*(desc as *const [u8; 0x38]);
                crate::xlog_warn!("XGCreateTextureComputer {hr:#x}: {:x?}", &d[..0x34]);
            }
            hr
        }
    }
}

unsafe extern "system" fn add_ref(p: *mut c_void) -> u32 {
    (*(p as *const Computer)).refs.fetch_add(1, Ordering::Relaxed) + 1
}

unsafe extern "system" fn release(p: *mut c_void) -> u32 {
    let n = (*(p as *const Computer)).refs.fetch_sub(1, Ordering::AcqRel) - 1;
    if n == 0 {
        drop(Box::from_raw(p as *mut Computer));
    }
    n
}

unsafe extern "system" fn get_resource_layout(p: *mut c_void, out: *mut u8) -> HRESULT {
    if out.is_null() {
        return E_POINTER;
    }
    tex(p).write(&mut *(out as *mut [u8; LAYOUT_BYTES]));
    S_OK
}

unsafe extern "system" fn get_size(p: *mut c_void) -> u64 {
    tex(p).size()
}

unsafe extern "system" fn get_alignment(p: *mut c_void) -> u64 {
    tex(p).alignment()
}

unsafe extern "system" fn get_mip_offset(p: *mut c_void, plane: u32, mip: u32) -> u64 {
    match tex(p).plane_surface(plane) {
        Some((s, at)) if (mip as usize) < s.chain.levels.len() => at + s.mip_offset(mip as usize),
        _ => 0,
    }
}

unsafe extern "system" fn get_texel_offset(p: *mut c_void, plane: u32, mip: u32, x: u64, y: u32, slice: u32, _sample: u32, bit: *mut u32) -> u64 {
    if !bit.is_null() {
        *bit = 0;
    }
    match tex(p).plane_surface(plane) {
        Some((s, at)) if (mip as usize) < s.chain.levels.len() && slice < s.array => at + s.element_offset(mip as usize, x as usize, y as usize, slice as usize),
        _ => 0,
    }
}

unsafe extern "system" fn not_implemented(_p: *mut c_void) -> HRESULT {
    E_NOTIMPL
}

/// (surface, its offset, level, slice) of a subresource (mip + slice × mips) of a plane
fn subresource(t: &Texture, plane: u32, sub: u32) -> Option<(&Surface, u64, usize, usize)> {
    let (s, at) = t.plane_surface(plane)?;
    let mips = s.chain.levels.len();
    let (i, k) = (sub as usize % mips, sub as usize / mips);
    (k < s.array as usize).then_some((s, at, i, k))
}

/// rows of `src` (`row_pitch` apart) → the level's place in `dst` (the whole resource)
unsafe extern "system" fn copy_into(p: *mut c_void, dst: *mut u8, plane: u32, sub: u32, src: *const u8, row_pitch: u32, _slice_pitch: u32) -> HRESULT {
    let Some((t, at, i, k)) = subresource(tex(p), plane, sub).filter(|_| !dst.is_null() && !src.is_null()) else { return E_INVALIDARG };
    let m = t.chain.levels[i];
    let row = m.width * t.bpe as usize;
    let mut linear = vec![0u8; row * m.height];
    for y in 0..m.height {
        linear[y * row..(y + 1) * row].copy_from_slice(std::slice::from_raw_parts(src.add(y * row_pitch as usize), row));
    }
    let slice = std::slice::from_raw_parts_mut(dst.add(at as usize + k * t.chain.slice_bytes), t.chain.slice_bytes);
    t.chain.tile_level(slice, i, k, &linear);
    S_OK
}

/// the level's place in `src` (the whole resource) → rows of `dst` (`row_pitch` apart)
unsafe extern "system" fn copy_from(p: *mut c_void, dst: *mut u8, plane: u32, sub: u32, src: *const u8, row_pitch: u32, _slice_pitch: u32) -> HRESULT {
    let Some((t, at, i, k)) = subresource(tex(p), plane, sub).filter(|_| !dst.is_null() && !src.is_null()) else { return E_INVALIDARG };
    let m = t.chain.levels[i];
    let row = m.width * t.bpe as usize;
    let slice = std::slice::from_raw_parts(src.add(at as usize + k * t.chain.slice_bytes), t.chain.slice_bytes);
    let linear = t.chain.detile_level(slice, i, k);
    for y in 0..m.height {
        std::ptr::copy_nonoverlapping(linear[y * row..].as_ptr(), dst.add(y * row_pitch as usize), row);
    }
    S_OK
}

#[cfg(test)]
mod tests {
    use super::*;

    fn desc(width: u64, height: u32, array: u16, mips: u16, format: u32, layout: u32) -> [u8; 0x38] {
        let mut d = [0u8; 0x38];
        d[0..4].copy_from_slice(&3u32.to_le_bytes());
        d[0x10..0x18].copy_from_slice(&width.to_le_bytes());
        d[0x18..0x1C].copy_from_slice(&height.to_le_bytes());
        d[0x1C..0x1E].copy_from_slice(&array.to_le_bytes());
        d[0x1E..0x20].copy_from_slice(&mips.to_le_bytes());
        d[0x20..0x24].copy_from_slice(&format.to_le_bytes());
        d[0x24..0x28].copy_from_slice(&1u32.to_le_bytes());
        d[0x2C..0x30].copy_from_slice(&layout.to_le_bytes());
        d
    }

    /// As the console's library answers (seen on it): RGBA8 256×256, full chain, 64KB_S.
    #[test]
    fn rgba8_64kb_s_layout() {
        let t = Texture::new(&desc(256, 256, 1, 0, 28, 0x109)).unwrap();
        let mut b = [0u8; LAYOUT_BYTES];
        t.write(&mut b);
        let q = |o: usize| u64::from_le_bytes(b[o..o + 8].try_into().unwrap());
        let w = |o: usize| u32::from_le_bytes(b[o..o + 4].try_into().unwrap());
        assert_eq!((q(0), q(8), w(0x10), w(PLANE + 0x2C), w(PLANE + 0x30)), (0x60000, 0x10000, 9, 2, 7));
        let mip = |i: usize, k: usize| w(PLANE + MIP + i * MIP_BYTES + 4 * k);
        let offsets: Vec<u32> = (0..9).map(|i| mip(i, 2)).collect();
        assert_eq!(offsets, [0x20000, 0x10000, 0x8000, 0x4000, 0x2000, 0x1000, 0x800, 0x600, 0x500]);
        assert_eq!((mip(2, 0), mip(2, 9), mip(2, 10), mip(2, 18), mip(6, 9), mip(6, 10)), (0x8000, 0x40, 0x80, 1, 8, 8));
        assert_eq!(optimal_mode(0, 0, 32, 256, 256), 5);
    }
}
