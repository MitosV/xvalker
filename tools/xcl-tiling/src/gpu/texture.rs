//! Console textures from console memory → PC textures. On the console a texture is the
//! memory a T# points at: the title writes it already tiled (GFX10 swizzle, mip chain and
//! slices as addrlib lays them out – its archives store textures that way) and samples it
//! directly. A PC texture has an opaque layout, so every mip level of every slice is detiled
//! (`tiling::Chain`) into a D3D12 subresource and copied in. SW_LINEAR resources use
//! the GFX10 256-byte row, level and slice alignment directly.
//!
//! 2D textures, arrays and cubes, 3D textures (thick and thin modes) and linear ones, the _X
//! modes with the console's pipe configuration (`console_pipes`). What is not here yet: MSAA,
//! and the binding to the console API that creates textures in title memory (and tells when
//! the title rewrote one) – that comes with the game executable.

use super::desc::{self, Image, ImageType};
use super::tiling::{self, Chain, PipeConfig};
use crate::d3d12::*;

/// Pipes / packers of the console's GB_ADDR_CONFIG – only the XOR (_X) modes depend on it.
/// From the console's own layout library: every texel of every mode matches with 32 pipes /
/// 8 packers by default (Series X), 8 pipes / 4 packers with its hardware version 4 (taken
/// to be Series S).
pub const CONSOLE_PIPES: PipeConfig = PipeConfig { pipes_log2: 5, pkrs_log2: 3 };
pub const SERIES_S_PIPES: PipeConfig = PipeConfig { pipes_log2: 3, pkrs_log2: 2 };

/// The pipe configuration of the console XCL reports to the title (`XSystemGetDeviceType`).
pub fn console_pipes() -> PipeConfig {
    if crate::state::config().is_series_s() {
        SERIES_S_PIPES
    } else {
        CONSOLE_PIPES
    }
}

/// Linear data of one subresource: `depth` slices (3D, else 1) of `rows` rows of `row_bytes`
/// (elements: pixels or 4×4 blocks).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Subresource {
    pub rows: usize,
    pub row_bytes: usize,
    pub depth: usize,
    pub data: Vec<u8>,
}

/// Where and how a T#'s texture lies in console memory.
pub struct Placement {
    /// first byte (the T# address rounded down to a block; the rest is the base swizzle)
    pub start: u64,
    pub bytes: usize,
    pub chain: Chain,
    /// array slices (cube faces), or the storage layers of a 3D texture.
    pub slices: usize,
}

pub fn placement(t: &Image, cfg: PipeConfig) -> Result<Placement, String> {
    if !desc::valid_image_shape(t) {
        return Err(format!("invalid {:?} texture dimensions, mip range or array view", t.ty));
    }
    if !desc::valid_cube_view(t) {
        return Err(format!("cube view {}..{} is not a nonempty whole number of faces", t.base_array, t.depth));
    }
    if matches!(t.ty, ImageType::D2Msaa | ImageType::D2MsaaArray) {
        return Err(format!("texture {:?} – not yet (MSAA)", t.ty));
    }
    let mut surface = *t;
    if matches!(surface.ty, ImageType::D1 | ImageType::D1Array) {
        // The T# height field is ignored for 1D resources, just as image_d3d12 ignores it.
        surface.height = 1;
    }
    // linear (SW_MODE 0) included: addrlib's 256-byte rows, levels from the smallest
    let chain = tiling::image_chain(&surface, cfg).ok_or_else(|| format!("SW_MODE {} / format {} without a tiling pattern", t.swizzle_mode, t.format))?;
    let slices = match t.ty {
        ImageType::D1Array | ImageType::D2Array | ImageType::Cube => t.depth.max(1) as usize,
        ImageType::D3 => chain.layers(),
        _ => 1,
    };
    let start = t.address & !(chain.layout.block_bytes as u64 - 1);
    let bytes = chain.slice_bytes.checked_mul(slices).ok_or("tiled texture size overflow")?;
    Ok(Placement { start, bytes, chain, slices })
}

/// The subresources of `t` in D3D12 order (mip + slice × mips) from `mem`, its bytes from
/// `Placement::start` on.
pub fn subresources(t: &Image, mem: &[u8], cfg: PipeConfig) -> Result<Vec<Subresource>, String> {
    let p = placement(t, cfg)?;
    if mem.len() < p.bytes {
        return Err(format!("{} B of memory, but the texture takes {} B", mem.len(), p.bytes));
    }
    let chain = &p.chain;
    let e = 1usize << chain.layout.elem_log2;
    if matches!(t.ty, ImageType::D3) {
        return Ok(chain
            .levels
            .iter()
            .enumerate()
            .map(|(i, l)| Subresource {
                rows: l.height,
                row_bytes: l.width * e,
                depth: l.depth,
                data: chain.detile_volume(mem, i),
            })
            .collect());
    }
    let mut out = Vec::with_capacity(p.slices * chain.levels.len());
    for s in 0..p.slices {
        let slice = &mem[s * chain.slice_bytes..(s + 1) * chain.slice_bytes];
        for (i, l) in chain.levels.iter().enumerate() {
            out.push(Subresource { depth: 1, rows: l.height, row_bytes: l.width * e, data: chain.detile_level(slice, i, s) });
        }
    }
    Ok(out)
}

/// The console memory a T#'s texture occupies, when it lies in the unified memory (UMA).
#[allow(dead_code)] // kept from XCL; its tests use it
fn checked_span(start: u64, bytes: usize) -> Result<(usize, usize), String> {
    if bytes == 0 {
        return Err("empty texture memory span".into());
    }
    let start = usize::try_from(start).map_err(|_| "texture address does not fit the host address space")?;
    let end = start.checked_add(bytes).ok_or("texture memory span overflows the host address space")?;
    Ok((start, end))
}
#[allow(dead_code)] // kept from XCL; its tests use it
fn validate_subresources(subs: &[Subresource]) -> Result<(), String> {
    if subs.is_empty() {
        return Err("texture upload without subresources".into());
    }
    for (i, s) in subs.iter().enumerate() {
        if s.depth == 0 || s.rows == 0 || s.row_bytes == 0 {
            return Err(format!("texture subresource {i} has a zero dimension"));
        }
        let bytes = s
            .depth
            .checked_mul(s.rows)
            .and_then(|n| n.checked_mul(s.row_bytes))
            .ok_or_else(|| format!("texture subresource {i} size overflow"))?;
        if s.data.len() < bytes {
            return Err(format!("texture subresource {i} has {} B, needs {bytes} B", s.data.len()));
        }
    }
    Ok(())
}

#[allow(dead_code)] // kept from XCL; its tests use it
fn validate_subresource_count(rd: &D3D12_RESOURCE_DESC, count: usize) -> Result<(), String> {
    let arrays = match rd.Dimension {
        D3D12_RESOURCE_DIMENSION_TEXTURE1D | D3D12_RESOURCE_DIMENSION_TEXTURE2D => rd.DepthOrArraySize as usize,
        D3D12_RESOURCE_DIMENSION_TEXTURE3D => 1,
        _ => return Err("texture upload requires a texture resource".into()),
    };
    // Every format accepted by desc::image_d3d12 is single-plane. Add the plane count here if
    // planar formats are introduced later.
    let expected = (rd.MipLevels as usize)
        .checked_mul(arrays)
        .ok_or("texture resource subresource count overflow")?;
    if count != expected {
        return Err(format!("texture upload has {count} subresources, resource requires {expected}"));
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;

    fn image(format: u32, ty: ImageType, w: u32, h: u32, slices: u32, mips: u32, mode: u32, address: u64) -> Image {
        Image {
            address,
            format,
            width: w,
            height: h,
            depth: slices,
            ty,
            base_level: 0,
            last_level: mips - 1,
            max_mip: mips - 1,
            base_array: 0,
            dst_sel: [4, 5, 6, 7],
            swizzle_mode: mode,
            min_lod: 0.0,
        }
    }

    #[test]
    fn texture_memory_spans_reject_empty_truncated_and_wrapping_addresses() {
        assert_eq!(checked_span(0x1000, 0x200).unwrap(), (0x1000, 0x1200));
        assert!(checked_span(0x1000, 0).unwrap_err().contains("empty"));
        assert!(checked_span(u64::MAX, 2).unwrap_err().contains("address"));
        assert!(checked_span((usize::MAX - 1) as u64, 2).unwrap_err().contains("overflows"));
        assert_eq!(checked_span((usize::MAX - 1) as u64, 1).unwrap(), (usize::MAX - 1, usize::MAX));
    }

    #[test]
    fn upload_subresources_are_validated_before_unsafe_copying() {
        assert!(validate_subresources(&[]).is_err());
        let zero = Subresource { depth: 1, rows: 0, row_bytes: 4, data: vec![] };
        assert!(validate_subresources(&[zero]).unwrap_err().contains("zero dimension"));
        let short = Subresource { depth: 2, rows: 3, row_bytes: 4, data: vec![0; 23] };
        assert_eq!(validate_subresources(&[short]).unwrap_err(), "texture subresource 0 has 23 B, needs 24 B");
        let overflow = Subresource { depth: usize::MAX, rows: 2, row_bytes: 1, data: vec![] };
        assert!(validate_subresources(&[overflow]).unwrap_err().contains("size overflow"));
        let complete = Subresource { depth: 2, rows: 3, row_bytes: 4, data: vec![0; 25] };
        assert!(validate_subresources(&[complete]).is_ok());
    }

    #[test]
    fn upload_requires_every_resource_subresource_exactly_once() {
        let mut rd = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            DepthOrArraySize: 3,
            MipLevels: 4,
            ..Default::default()
        };
        assert!(validate_subresource_count(&rd, 12).is_ok());
        assert_eq!(validate_subresource_count(&rd, 11).unwrap_err(), "texture upload has 11 subresources, resource requires 12");
        assert_eq!(validate_subresource_count(&rd, 13).unwrap_err(), "texture upload has 13 subresources, resource requires 12");
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.DepthOrArraySize = 9;
        assert!(validate_subresource_count(&rd, 4).is_ok());
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        assert!(validate_subresource_count(&rd, 4).unwrap_err().contains("requires a texture"));
    }

    /// Random linear subresources for `t`, and the console memory they are, tiled.
    fn console_texture(t: &Image) -> (Vec<Subresource>, Vec<u8>) {
        let p = placement(t, CONSOLE_PIPES).unwrap();
        let mut mem = vec![0u8; p.bytes];
        let mut subs = Vec::new();
        let mut n = 0x1234_5678u32;
        let mut random = |len: usize| -> Vec<u8> {
            (0..len)
                .map(|_| {
                    n ^= n << 13;
                    n ^= n >> 17;
                    n ^= n << 5;
                    n as u8
                })
                .collect()
        };
        let chain = &p.chain;
        let e = 1usize << chain.layout.elem_log2;
        if matches!(t.ty, ImageType::D3) {
            for (i, l) in chain.levels.iter().enumerate() {
                let data = random(l.width * l.height * l.depth * e);
                chain.tile_volume(&mut mem, i, &data);
                subs.push(Subresource { rows: l.height, row_bytes: l.width * e, depth: l.depth, data });
            }
            return (subs, mem);
        }
        for s in 0..p.slices {
            let slice = &mut mem[s * chain.slice_bytes..(s + 1) * chain.slice_bytes];
            for (i, l) in chain.levels.iter().enumerate() {
                let data = random(l.width * l.height * e);
                chain.tile_level(slice, i, s, &data);
                subs.push(Subresource { depth: 1, rows: l.height, row_bytes: l.width * e, data });
            }
        }
        (subs, mem)
    }

    #[test]
    fn subresources_of_a_mipped_array_from_console_memory() {
        let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
        let t = image(rgba8, ImageType::D2Array, 96, 40, 3, 6, 5, 0x10_0000 | 0x300);
        let (want, mem) = console_texture(&t);
        let p = placement(&t, CONSOLE_PIPES).unwrap();
        assert_eq!((p.start, p.slices, want.len()), (0x10_0000, 3, 18));
        assert_eq!(subresources(&t, &mem, CONSOLE_PIPES).unwrap(), want);
        assert!(subresources(&t, &mem[..mem.len() - 1], CONSOLE_PIPES).is_err());
    }
    #[test]
    fn one_dimensional_arrays_ignore_tsharp_height_for_linear_and_tiled_layouts() {
        let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
        for mode in [0, 1] {
            let t = image(rgba8, ImageType::D1Array, 33, 37, 2, 3, mode, 0x36_0000);
            let mut normalized = t;
            normalized.height = 1;
            assert_eq!(placement(&t, CONSOLE_PIPES).unwrap().bytes, placement(&normalized, CONSOLE_PIPES).unwrap().bytes);
            let (want, mem) = console_texture(&t);
            assert!(want.iter().all(|s| s.rows == 1));
            assert_eq!(subresources(&t, &mem, CONSOLE_PIPES).unwrap(), want);
        }
    }
    #[test]
    fn non_array_2d_ignores_tsharp_depth_for_linear_and_tiled_uploads() {
        let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
        for mode in [0, 1] {
            let t = image(rgba8, ImageType::D2, 17, 3, 9, 2, mode, 0x37_0000);
            assert_eq!(desc::image_d3d12(&t).unwrap().0.DepthOrArraySize, 1);
            assert_eq!(placement(&t, CONSOLE_PIPES).unwrap().slices, 1);
            let (want, mem) = console_texture(&t);
            assert_eq!(subresources(&t, &mem, CONSOLE_PIPES).unwrap(), want);
        }
    }
    #[test]
    fn invalid_cube_face_ranges_are_rejected_before_placement() {
        let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
        let mut t = image(rgba8, ImageType::Cube, 16, 16, 5, 1, 1, 0x37_8000);
        assert!(placement(&t, CONSOLE_PIPES).err().unwrap().contains("whole number of faces"));
        t.depth = 12;
        t.base_array = 12;
        assert!(placement(&t, CONSOLE_PIPES).err().unwrap().contains("whole number of faces"));
        t.base_array = 6;
        assert!(placement(&t, CONSOLE_PIPES).is_ok());
    }

    #[test]
    fn invalid_mip_counts_are_rejected_before_layout_shifts() {
        let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
        let mut t = image(rgba8, ImageType::D2, 8, 4, 1, 4, 0, 0x37_c000);
        assert!(placement(&t, CONSOLE_PIPES).is_ok());
        t.max_mip = 32;
        t.last_level = 32;
        assert!(placement(&t, CONSOLE_PIPES).err().unwrap().contains("invalid"));
        t.max_mip = 4;
        t.last_level = 4;
        assert!(placement(&t, CONSOLE_PIPES).is_err());
        t.max_mip = 3;
        t.last_level = 2;
        t.base_level = 3;
        assert!(placement(&t, CONSOLE_PIPES).is_err());
    }

    #[test]
    fn oversized_array_depth_is_rejected_before_placement_allocation() {
        let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
        let t = image(rgba8, ImageType::D2Array, 1, 1, u16::MAX as u32 + 1, 1, 1, 0x37_d000);
        assert!(placement(&t, CONSOLE_PIPES).err().unwrap().contains("invalid"));
        assert!(desc::image_d3d12(&t).is_none());
    }
}
