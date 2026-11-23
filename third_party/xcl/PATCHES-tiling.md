# Patches to the vendored XCL tiling files

The files under `src/` are byte-identical with XCL commit `db5dc26` (see README-tiling.md);
none of them is edited. What the runtime builds differs from them as recorded here.

## tools/xcl-tiling (the crate, `xcl_tiling.dll`)

Included byte-identical from `src/` via `#[path]`: `gpu/tiling.rs`, `gpu/tiling_tables.rs`,
`hle/xg.rs` (their `#[cfg(test)]` modules run as the crate's tests). The crate provides the
XCL terms they reference: `util::{HRESULT, S_OK}`, `state::config()` (Series S flag, default
off), `stubs::make_stub_info` (an E_NOTIMPL stub), the `hle_exports!`, `xlog_warn!`,
`xlog_info!` macros — all in `src/lib.rs`, none of it XCL code.

`gpu/desc.rs` and `gpu/texture.rs` are copies with the edits below. Their `windows` terms
become `crate::d3d12` (`src/d3d12.rs`, generated from the Windows SDK headers by
`gen_d3d12.py`: the same names and values, plain data, no COM). The GPU upload paths of
`texture.rs` are left out — the runtime does those itself (`title_textures.cpp`); the pure
parts the brief names (`placement`, `subresources`, `console_pipes`) and XCL's own tests of
them are kept verbatim. New code, not from XCL: `src/abi.rs` (the C ABI of
`src/runtime/modules/xcl_tiling.h`), `src/gpu/mod.rs`, `src/hle/mod.rs`, `build.sh`,
`Cargo.toml`, `gen_d3d12.py`.

### gpu/desc.rs — the two `windows` imports become the local terms

```diff
--- third_party/xcl/src/gpu/desc.rs	2026-11-23 04:12:52.597912049 -0600
+++ tools/xcl-tiling/src/gpu/desc.rs	2026-11-23 04:31:31.078224449 -0600
@@ -4,8 +4,7 @@
 //! a cache keyed by the raw words …) can use them. Field positions as in AMD's GFX10
 //! register docs, cross-checked with AnyPS5 (GPL-2.0); format numbers = GFX10 IMG_FORMAT.
 
-use windows::Win32::Graphics::Direct3D12::*;
-use windows::Win32::Graphics::Dxgi::Common::*;
+use crate::d3d12::*;
 
 /// GFX10 image TYPE (T# word 3 [31:28]).
 #[derive(Debug, Clone, Copy, PartialEq, Eq)]
```

### gpu/texture.rs — imports, dead-code allows for the kept-but-unused validators, the GPU paths out

Kept verbatim: the module doc, `CONSOLE_PIPES` / `SERIES_S_PIPES` / `console_pipes`,
`Subresource`, `Placement`, `placement`, `subresources`, `checked_span`,
`validate_subresources`, `validate_subresource_count`, and the tests of the pure parts
(the two 1D / non-array tests lose only their GPU half; the TTPF archive test and the GPU
read-back test go with the upload paths). Dropped: `uma_bytes` (XCL's UMA), `err`, `create`,
`SHADER_READ`, `upload`, `upload_batch`, `record_upload`, `wait`, `buffer`, `transition`,
`device`, `read_back`, `gpu_console_textures_upload_every_mip_and_slice`, `mip_error`,
`retail_ttpf_textures_detile_with_their_mip_chains`, `roughness`, `ttpf_element`, `TtpfTexture`,
`Ttpf` — everything that needs a D3D12 device or XCL's UMA / sample archives.

```diff
--- third_party/xcl/src/gpu/texture.rs	2026-11-23 04:12:52.596912064 -0600
+++ tools/xcl-tiling/src/gpu/texture.rs	2026-11-23 04:39:09.650423985 -0600
@@ -12,7 +12,7 @@
 
 use super::desc::{self, Image, ImageType};
 use super::tiling::{self, Chain, PipeConfig};
-use windows::Win32::Graphics::Direct3D12::*;
+use crate::d3d12::*;
 
 /// Pipes / packers of the console's GB_ADDR_CONFIG – only the XOR (_X) modes depend on it.
 /// From the console's own layout library: every texel of every mode matches with 32 pipes /
@@ -110,6 +110,7 @@
 }
 
 /// The console memory a T#'s texture occupies, when it lies in the unified memory (UMA).
+#[allow(dead_code)] // kept from XCL; its tests use it
 fn checked_span(start: u64, bytes: usize) -> Result<(usize, usize), String> {
     if bytes == 0 {
         return Err("empty texture memory span".into());
@@ -118,38 +119,7 @@
     let end = start.checked_add(bytes).ok_or("texture memory span overflows the host address space")?;
     Ok((start, end))
 }
-
-pub fn uma_bytes(t: &Image, cfg: PipeConfig) -> Result<&'static [u8], String> {
-    let p = placement(t, cfg)?;
-    let u = super::uma::get().ok_or("no UMA")?;
-    let (a, end) = checked_span(p.start, p.bytes)?;
-    if !u.contains(a) || !u.contains(end - 1) {
-        return Err(format!("texture {a:#x}+{:#x} outside UMA", p.bytes));
-    }
-    // SAFETY: inside the UMA reservation, which lives for the whole process
-    Ok(unsafe { std::slice::from_raw_parts(a as *const u8, p.bytes) })
-}
-
-fn err(x: windows::core::Error) -> String {
-    format!("{x}")
-}
-
-/// A PC texture (default heap, left in the shader-resource states) with the console
-/// texture's contents: `mem` from `Placement::start` on.
-pub unsafe fn create(dev: &ID3D12Device, queue: &ID3D12CommandQueue, t: &Image, mem: &[u8], cfg: PipeConfig) -> Result<ID3D12Resource, String> {
-    let subs = subresources(t, mem, cfg)?;
-    let (rd, _) = desc::image_d3d12(t).ok_or_else(|| format!("format {} without a DXGI equivalent", t.format))?;
-    let mut r: Option<ID3D12Resource> = None;
-    dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_DEFAULT, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut r)
-        .map_err(|x| format!("CreateCommittedResource: {x}"))?;
-    let r = r.ok_or("CreateCommittedResource without a resource")?;
-    upload(dev, queue, &r, &subs, D3D12_RESOURCE_STATE_COPY_DEST, SHADER_READ)?;
-    Ok(r)
-}
-
-/// The states `create` leaves a texture in.
-pub const SHADER_READ: D3D12_RESOURCE_STATES = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE.0 | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE.0);
-
+#[allow(dead_code)] // kept from XCL; its tests use it
 fn validate_subresources(subs: &[Subresource]) -> Result<(), String> {
     if subs.is_empty() {
         return Err("texture upload without subresources".into());
@@ -170,6 +140,7 @@
     Ok(())
 }
 
+#[allow(dead_code)] // kept from XCL; its tests use it
 fn validate_subresource_count(rd: &D3D12_RESOURCE_DESC, count: usize) -> Result<(), String> {
     let arrays = match rd.Dimension {
         D3D12_RESOURCE_DIMENSION_TEXTURE1D | D3D12_RESOURCE_DIMENSION_TEXTURE2D => rd.DepthOrArraySize as usize,
@@ -186,148 +157,9 @@
     }
     Ok(())
 }
-
-/// Copy `subs` (all subresources, D3D12 order) into `res` on `queue` and wait; `res` is in
-/// `before` and ends in `after` (e.g. again after the title rewrote the memory).
-pub unsafe fn upload(dev: &ID3D12Device, queue: &ID3D12CommandQueue, res: &ID3D12Resource, subs: &[Subresource], before: D3D12_RESOURCE_STATES, after: D3D12_RESOURCE_STATES) -> Result<(), String> {
-    let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).map_err(err)?;
-    let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).map_err(err)?;
-    let staging = record_upload(dev, &list, res, subs, before, after)?;
-    list.Close().map_err(err)?;
-    queue.ExecuteCommandLists(&[Some(windows::core::Interface::cast(&list).map_err(err)?)]);
-    let r = wait(dev, queue);
-    drop(staging);
-    r
-}
-
-/// Several uploads on one list, one wait: (resource, subresources, before, after) each. A
-/// failure of one is returned with its index; the others still run.
-pub unsafe fn upload_batch(dev: &ID3D12Device, queue: &ID3D12CommandQueue, items: &[(ID3D12Resource, Vec<Subresource>, D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES)]) -> Result<Vec<(usize, String)>, String> {
-    if items.is_empty() {
-        return Ok(Vec::new());
-    }
-    let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).map_err(err)?;
-    let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).map_err(err)?;
-    let mut staging = Vec::with_capacity(items.len());
-    let mut failed = Vec::new();
-    for (k, (res, subs, before, after)) in items.iter().enumerate() {
-        match record_upload(dev, &list, res, subs, *before, *after) {
-            Ok(s) => staging.push(s),
-            Err(e) => failed.push((k, e)),
-        }
-    }
-    list.Close().map_err(err)?;
-    queue.ExecuteCommandLists(&[Some(windows::core::Interface::cast(&list).map_err(err)?)]);
-    wait(dev, queue)?;
-    drop(staging);
-    Ok(failed)
-}
-
-/// Record the copy of `subs` into `res` on `list` (barriers from `before` and back to `after`);
-/// the staging buffer returned must live until the list has run.
-pub unsafe fn record_upload(dev: &ID3D12Device, list: &ID3D12GraphicsCommandList, res: &ID3D12Resource, subs: &[Subresource], before: D3D12_RESOURCE_STATES, after: D3D12_RESOURCE_STATES) -> Result<ID3D12Resource, String> {
-    validate_subresources(subs)?;
-    let rd = res.GetDesc();
-    let n = subs.len();
-    validate_subresource_count(&rd, n)?;
-    let mut fps = vec![D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default(); n];
-    let mut rows = vec![0u32; n];
-    let mut row_bytes = vec![0u64; n];
-    let mut total = 0u64;
-    dev.GetCopyableFootprints(&rd, 0, n as u32, 0, Some(fps.as_mut_ptr()), Some(rows.as_mut_ptr()), Some(row_bytes.as_mut_ptr()), Some(&mut total));
-    for (i, ((s, fp), (&nrows, &nbytes))) in subs.iter().zip(&fps).zip(rows.iter().zip(&row_bytes)).enumerate() {
-        if s.rows != nrows as usize || s.depth != fp.Footprint.Depth as usize || s.row_bytes != nbytes as usize {
-            return Err(format!(
-                "texture subresource {i} is {}x{}x{} B, resource expects {}x{}x{} B",
-                s.row_bytes, s.rows, s.depth, nbytes, nrows, fp.Footprint.Depth
-            ));
-        }
-    }
-    let up = buffer(dev, total)?;
-    let mut p = std::ptr::null_mut();
-    up.Map(0, None, Some(&mut p)).map_err(err)?;
-    for ((s, fp), &nrows) in subs.iter().zip(&fps).zip(&rows) {
-        let pitch = fp.Footprint.RowPitch as usize;
-        let row = s.row_bytes.min(pitch);
-        // depth slices are NumRows rows apart (rows of 4×4 blocks for BCn)
-        let slice_pitch = pitch * nrows as usize;
-        for z in 0..(fp.Footprint.Depth as usize).min(s.depth) {
-            for y in 0..(nrows as usize).min(s.rows) {
-                let at = fp.Offset as usize + z * slice_pitch + y * pitch;
-                std::ptr::copy_nonoverlapping(s.data[(z * s.rows + y) * s.row_bytes..].as_ptr(), (p as *mut u8).add(at), row);
-            }
-        }
-    }
-    up.Unmap(0, None);
-    if before != D3D12_RESOURCE_STATE_COPY_DEST {
-        list.ResourceBarrier(&[transition(res, before, D3D12_RESOURCE_STATE_COPY_DEST)]);
-    }
-    for (k, fp) in fps.iter().enumerate() {
-        let dst = D3D12_TEXTURE_COPY_LOCATION {
-            pResource: std::mem::transmute_copy(res),
-            Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
-            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: k as u32 },
-        };
-        let src = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&up), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: *fp } };
-        list.CopyTextureRegion(&dst, 0, 0, 0, &src, None);
-    }
-    if after != D3D12_RESOURCE_STATE_COPY_DEST {
-        list.ResourceBarrier(&[transition(res, D3D12_RESOURCE_STATE_COPY_DEST, after)]);
-    }
-    Ok(up)
-}
-
-/// Wait until `queue` has run everything submitted so far.
-pub unsafe fn wait(dev: &ID3D12Device, queue: &ID3D12CommandQueue) -> Result<(), String> {
-    let fence: ID3D12Fence = dev.CreateFence(0, D3D12_FENCE_FLAG_NONE).map_err(err)?;
-    queue.Signal(&fence, 1).map_err(err)?;
-    let ev = windows::Win32::System::Threading::CreateEventW(None, false, false, None).map_err(err)?;
-    fence.SetEventOnCompletion(1, ev).map_err(err)?;
-    windows::Win32::System::Threading::WaitForSingleObject(ev, 10_000);
-    let _ = windows::Win32::Foundation::CloseHandle(ev);
-    if fence.GetCompletedValue() < 1 {
-        return Err(format!("GPU did not finish the texture copy within 10 s ({:?})", dev.GetDeviceRemovedReason()));
-    }
-    Ok(())
-}
-
-unsafe fn buffer(dev: &ID3D12Device, size: u64) -> Result<ID3D12Resource, String> {
-    let rd = D3D12_RESOURCE_DESC {
-        Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
-        Width: size.max(256),
-        Height: 1,
-        DepthOrArraySize: 1,
-        MipLevels: 1,
-        SampleDesc: windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
-        Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
-        ..Default::default()
-    };
-    let mut r: Option<ID3D12Resource> = None;
-    dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_UPLOAD, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, None, &mut r)
-        .map_err(|x| format!("texture upload buffer: {x}"))?;
-    r.ok_or_else(|| "texture upload buffer".into())
-}
-
-fn transition(r: &ID3D12Resource, before: D3D12_RESOURCE_STATES, after: D3D12_RESOURCE_STATES) -> D3D12_RESOURCE_BARRIER {
-    D3D12_RESOURCE_BARRIER {
-        Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
-        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
-        Anonymous: D3D12_RESOURCE_BARRIER_0 {
-            Transition: std::mem::ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER {
-                pResource: unsafe { std::mem::transmute_copy(r) },
-                Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
-                StateBefore: before,
-                StateAfter: after,
-            }),
-        },
-    }
-}
-
 #[cfg(test)]
 mod tests {
     use super::*;
-    use std::collections::HashMap;
-    use windows::Win32::Graphics::Dxgi::Common::*;
 
     fn image(format: u32, ty: ImageType, w: u32, h: u32, slices: u32, mips: u32, mode: u32, address: u64) -> Image {
         Image {
@@ -434,11 +266,9 @@
         assert_eq!(subresources(&t, &mem, CONSOLE_PIPES).unwrap(), want);
         assert!(subresources(&t, &mem[..mem.len() - 1], CONSOLE_PIPES).is_err());
     }
-
     #[test]
     fn one_dimensional_arrays_ignore_tsharp_height_for_linear_and_tiled_layouts() {
         let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
-        let gpu = device();
         for mode in [0, 1] {
             let t = image(rgba8, ImageType::D1Array, 33, 37, 2, 3, mode, 0x36_0000);
             let mut normalized = t;
@@ -447,34 +277,19 @@
             let (want, mem) = console_texture(&t);
             assert!(want.iter().all(|s| s.rows == 1));
             assert_eq!(subresources(&t, &mem, CONSOLE_PIPES).unwrap(), want);
-            if let Some((dev, queue)) = &gpu {
-                unsafe {
-                    let res = create(dev, queue, &t, &mem, CONSOLE_PIPES).unwrap();
-                    assert_eq!(read_back(dev, queue, &res, &want), want);
-                }
-            }
         }
     }
-
     #[test]
     fn non_array_2d_ignores_tsharp_depth_for_linear_and_tiled_uploads() {
         let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
-        let gpu = device();
         for mode in [0, 1] {
             let t = image(rgba8, ImageType::D2, 17, 3, 9, 2, mode, 0x37_0000);
             assert_eq!(desc::image_d3d12(&t).unwrap().0.DepthOrArraySize, 1);
             assert_eq!(placement(&t, CONSOLE_PIPES).unwrap().slices, 1);
             let (want, mem) = console_texture(&t);
             assert_eq!(subresources(&t, &mem, CONSOLE_PIPES).unwrap(), want);
-            if let Some((dev, queue)) = &gpu {
-                unsafe {
-                    let res = create(dev, queue, &t, &mem, CONSOLE_PIPES).unwrap();
-                    assert_eq!(read_back(dev, queue, &res, &want), want);
-                }
-            }
         }
     }
-
     #[test]
     fn invalid_cube_face_ranges_are_rejected_before_placement() {
         let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
@@ -511,353 +326,4 @@
         assert!(placement(&t, CONSOLE_PIPES).err().unwrap().contains("invalid"));
         assert!(desc::image_d3d12(&t).is_none());
     }
-
-    fn device() -> Option<(ID3D12Device, ID3D12CommandQueue)> {
-        unsafe {
-            let mut dev: Option<ID3D12Device> = None;
-            windows::Win32::Graphics::Direct3D12::D3D12CreateDevice(None, windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0, &mut dev).ok()?;
-            let dev = dev?;
-            let queue = dev.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC { Type: D3D12_COMMAND_LIST_TYPE_DIRECT, ..Default::default() }).ok()?;
-            Some((dev, queue))
-        }
-    }
-
-    /// Every subresource of a PC texture, read back (rows of `row_bytes`).
-    unsafe fn read_back(dev: &ID3D12Device, queue: &ID3D12CommandQueue, res: &ID3D12Resource, subs: &[Subresource]) -> Vec<Subresource> {
-        let rd = res.GetDesc();
-        let n = subs.len();
-        let mut fps = vec![D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default(); n];
-        let mut nrows = vec![0u32; n];
-        let mut total = 0u64;
-        dev.GetCopyableFootprints(&rd, 0, n as u32, 0, Some(fps.as_mut_ptr()), Some(nrows.as_mut_ptr()), None, Some(&mut total));
-        let mut rb: Option<ID3D12Resource> = None;
-        let bd = D3D12_RESOURCE_DESC { Dimension: D3D12_RESOURCE_DIMENSION_BUFFER, Width: total, Height: 1, DepthOrArraySize: 1, MipLevels: 1, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 }, Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR, ..Default::default() };
-        dev.CreateCommittedResource(&D3D12_HEAP_PROPERTIES { Type: D3D12_HEAP_TYPE_READBACK, ..Default::default() }, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, None, &mut rb).unwrap();
-        let rb = rb.unwrap();
-        let alloc: ID3D12CommandAllocator = dev.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT).unwrap();
-        let list: ID3D12GraphicsCommandList = dev.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &alloc, None).unwrap();
-        list.ResourceBarrier(&[transition(res, SHADER_READ, D3D12_RESOURCE_STATE_COPY_SOURCE)]);
-        for (k, fp) in fps.iter().enumerate() {
-            let src = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(res), Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: k as u32 } };
-            let dst = D3D12_TEXTURE_COPY_LOCATION { pResource: std::mem::transmute_copy(&rb), Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { PlacedFootprint: *fp } };
-            list.CopyTextureRegion(&dst, 0, 0, 0, &src, None);
-        }
-        list.ResourceBarrier(&[transition(res, D3D12_RESOURCE_STATE_COPY_SOURCE, SHADER_READ)]);
-        list.Close().unwrap();
-        queue.ExecuteCommandLists(&[Some(windows::core::Interface::cast(&list).unwrap())]);
-        wait(dev, queue).unwrap();
-        let mut p = std::ptr::null_mut();
-        rb.Map(0, None, Some(&mut p)).unwrap();
-        let out = subs
-            .iter()
-            .zip(fps.iter().zip(&nrows))
-            .map(|(s, (fp, &nr))| {
-                let pitch = fp.Footprint.RowPitch as usize;
-                let mut data = Vec::with_capacity(s.depth * s.rows * s.row_bytes);
-                for z in 0..s.depth {
-                    for y in 0..s.rows {
-                        data.extend_from_slice(std::slice::from_raw_parts((p as *const u8).add(fp.Offset as usize + (z * nr as usize + y) * pitch), s.row_bytes));
-                    }
-                }
-                Subresource { rows: s.rows, row_bytes: s.row_bytes, depth: s.depth, data }
-            })
-            .collect();
-        rb.Unmap(0, None);
-        out
-    }
-
-    /// Tiled console memory → PC texture → read back = the linear data it was tiled from, for
-    /// every mip and slice: an RGBA8 array with 4 KB tiles, a BC7 cube with 64 KB tiles, 3D
-    /// textures (thick 4 KB / 64 KB, BC1 thick, linear) and a linear 2D texture.
-    #[test]
-    fn gpu_console_textures_upload_every_mip_and_slice() {
-        let Some((dev, queue)) = device() else {
-            println!("no D3D12 device – skipping");
-            crate::gpu::recomp::runner::skip_test("no GPU");
-            return;
-        };
-        let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
-        let bc7 = desc::gfx10_format(DXGI_FORMAT_BC7_UNORM).unwrap();
-        let bc1 = desc::gfx10_format(DXGI_FORMAT_BC1_UNORM).unwrap();
-        let rgba16f = desc::gfx10_format(DXGI_FORMAT_R16G16B16A16_FLOAT).unwrap();
-        for t in [
-            image(rgba8, ImageType::D2Array, 200, 72, 3, 8, 5, 0x40_0000),
-            image(bc7, ImageType::Cube, 256, 256, 6, 9, 10, 0x80_0000),
-            image(rgba8, ImageType::D3, 40, 24, 20, 6, 5, 0x100_0000),
-            image(rgba16f, ImageType::D3, 64, 64, 64, 7, 9, 0x200_0000),
-            image(bc1, ImageType::D3, 64, 32, 8, 4, 5, 0x300_0000),
-            image(rgba8, ImageType::D3, 33, 17, 5, 3, 0, 0x400_0000),
-            image(rgba8, ImageType::D2, 100, 30, 1, 7, 0, 0x500_0000),
-        ] {
-            let (want, mem) = console_texture(&t);
-            unsafe {
-                let res = create(&dev, &queue, &t, &mem, CONSOLE_PIPES).unwrap();
-                let got = read_back(&dev, &queue, &res, &want);
-                for (k, (g, w)) in got.iter().zip(&want).enumerate() {
-                    assert!(g == w, "format {} {:?} mode {} subresource {k} ({} × {} rows × {} B) differs", t.format, t.ty, t.swizzle_mode, w.depth, w.rows, w.row_bytes);
-                }
-            }
-        }
-    }
-
-    /// Mean |level `k` - level `k - 1` scaled down 2×| per channel (0..255 scale) of a texture
-    /// with `ch` channels of `cb` bytes (unorm); None when the sizes do not halve.
-    fn mip_error(big: &Subresource, small: &Subresource, ch: usize, cb: usize) -> Option<f64> {
-        let e = ch * cb;
-        let (bw, bh, sw, sh) = (big.row_bytes / e, big.rows, small.row_bytes / e, small.rows);
-        if sw * 2 != bw || sh * 2 != bh {
-            return None;
-        }
-        let get = |d: &[u8], w: usize, x: usize, y: usize, c: usize| -> u32 {
-            let o = (y * w + x) * e + c * cb;
-            if cb == 1 {
-                d[o] as u32
-            } else {
-                u16::from_le_bytes([d[o], d[o + 1]]) as u32 >> 8
-            }
-        };
-        let mut sum = 0u64;
-        for y in 0..sh {
-            for x in 0..sw {
-                for c in 0..ch {
-                    let avg = (get(&big.data, bw, 2 * x, 2 * y, c) + get(&big.data, bw, 2 * x + 1, 2 * y, c) + get(&big.data, bw, 2 * x, 2 * y + 1, c) + get(&big.data, bw, 2 * x + 1, 2 * y + 1, c) + 2) / 4;
-                    sum += (avg as i32 - get(&small.data, sw, x, y, c) as i32).unsigned_abs() as u64;
-                }
-            }
-        }
-        Some(sum as f64 / (sw * sh * ch) as f64)
-    }
-
-    /// The retail TTPF archive (`XCL_SAMPLE_TTPF`): (1) the mip chain model gives the space
-    /// every texture takes; (2) on its uncompressed unorm textures each detiled mip level is
-    /// close to the level above it scaled down 2× (mip tail levels included), while the same
-    /// tail levels read from a wrong place in the tail block (origin 0, 0) are not.
-    #[test]
-    fn retail_ttpf_textures_detile_with_their_mip_chains() {
-        let Ok(path) = std::env::var("XCL_SAMPLE_TTPF") else {
-            println!("XCL_SAMPLE_TTPF not set – skipping");
-            return;
-        };
-        let a = Ttpf::open(&path);
-        let (mut fit, mut sized, mut unreadable) = (0, 0, 0);
-        let rgba8 = desc::gfx10_format(DXGI_FORMAT_R8G8B8A8_UNORM).unwrap();
-        let (mut checked, mut close, mut worst) = (0, 0, Vec::new());
-        let (mut tail_levels, mut tail_err, mut wrong_err) = (0, 0.0f64, 0.0f64);
-        let mut volumes = Vec::new();
-        for t in &a.textures {
-            let Some((e, side, _)) = ttpf_element(t.dxgi) else { continue };
-            let gfx10 = match (e, side) {
-                (0, 1) => 1,
-                (1, 1) => 7,
-                (2, 1) => rgba8,
-                (3, 1) => 62,
-                (3, 4) => 169,
-                _ => 171,
-            };
-            let ty = match t.dim {
-                3 => ImageType::D3,
-                2 => ImageType::Cube,
-                _ if t.slices > 1 => ImageType::D2Array,
-                _ => ImageType::D2,
-            };
-            let img = image(gfx10, ty, t.w, t.h, t.slices, t.mips, t.mode, 0);
-            let p = placement(&img, CONSOLE_PIPES).unwrap();
-            sized += 1;
-            if p.bytes == t.end - t.start || p.bytes.div_ceil(4096) * 4096 == t.end - t.start {
-                fit += 1;
-            }
-            // volumes of 16-bit float texels (colour-grading LUTs): neighbours along x and z
-            // differ little read as thick blocks, a lot read as 2D slices
-            if t.dim == 3 && matches!(t.dxgi, 10 | 54) {
-                if let Some(mem) = a.read(t.start, t.end) {
-                    let thick = subresources(&img, &mem, CONSOLE_PIPES).unwrap().remove(0);
-                    let flat = image(gfx10, ImageType::D2Array, t.w, t.h, t.slices, t.mips, t.mode, 0);
-                    let as_2d: Vec<u8> = subresources(&flat, &mem, CONSOLE_PIPES).unwrap().into_iter().flat_map(|s| s.data).collect();
-                    let (w, h, d, e) = (t.w as usize, t.h as usize, t.slices as usize, 1usize << e);
-                    volumes.push((t.index, roughness(&thick.data, w, h, d, e), roughness(&as_2d, w, h, d, e)));
-                }
-            }
-            // unorm channels: R8 / A8, RGBA8 / BGRA8, RGBA16
-            let (ch, cb) = match t.dxgi {
-                61 | 65 => (1, 1),
-                27..=29 | 87 | 90 | 91 => (4, 1),
-                11 => (4, 2),
-                _ => continue,
-            };
-            if t.mips < 2 || t.slices != 1 {
-                continue;
-            }
-            let Some(mem) = a.read(t.start, t.start + p.bytes) else {
-                unreadable += 1;
-                continue;
-            };
-            let subs = subresources(&img, &mem, CONSOLE_PIPES).unwrap();
-            let mut bad = 0.0f64;
-            for k in 1..subs.len() {
-                let Some(err) = mip_error(&subs[k - 1], &subs[k], ch, cb) else { continue };
-                bad = bad.max(err);
-                // control: a tail level read from the tail block's corner
-                let chain = &p.chain;
-                let l = chain.levels[k];
-                if l.origin != (0, 0) && l.width >= 2 && l.height >= 2 {
-                    let mut wrong = chain.clone();
-                    wrong.levels[k].origin = (0, 0);
-                    let ws = Subresource { depth: 1, rows: l.height, row_bytes: subs[k].row_bytes, data: wrong.detile_level(&mem, k, 0) };
-                    tail_levels += 1;
-                    tail_err += err;
-                    wrong_err += mip_error(&subs[k - 1], &ws, ch, cb).unwrap();
-                }
-            }
-            checked += 1;
-            if bad < 8.0 {
-                close += 1;
-            } else if worst.len() < 8 {
-                worst.push((t.index, t.w, t.h, t.mips, t.mode, bad));
-            }
-        }
-        let n = tail_levels.max(1) as f64;
-        println!("TTPF: the mip chain model size matches for {fit} of {sized} textures");
-        println!("TTPF: textures with mips (R8/RGBA8/RGBA16): {close} of {checked} – every mip close to the previous one downscaled (< 8/255), {unreadable} in unreadable blocks; outliers: {worst:?}");
-        println!("TTPF: {tail_levels} levels in the mip tail – error {:.2}/255, at the wrong place in the block {:.2}/255", tail_err / n, wrong_err / n);
-        println!("TTPF: volumes (index, neighbour difference as thick blocks, as 2D slices): {volumes:?}");
-        assert!(fit * 100 >= sized * 99, "mip chain model: {fit} / {sized}");
-        assert!(checked >= 5 && close * 100 >= checked * 90, "mips: {close} / {checked}");
-        assert!(tail_levels >= 5 && wrong_err > 3.0 * tail_err, "mip tail: {tail_err} vs {wrong_err}");
-        assert!(!volumes.is_empty() && volumes.iter().all(|v| v.2 > 3.0 * v.1), "volumes: {volumes:?}");
-    }
-
-    /// Mean |difference| between x and z neighbours of the first channel (half floats, `e`-byte
-    /// texels) of a `w` × `h` × `d` volume, relative to the mean |value|.
-    fn roughness(v: &[u8], w: usize, h: usize, d: usize, e: usize) -> f64 {
-        let half = |b: u16| -> f64 {
-            let (s, x, m) = ((b >> 15) as i32, ((b >> 10) & 31) as i32, (b & 1023) as f64);
-            let v = if x == 0 { m / 1024.0 * 2f64.powi(-14) } else { (1.0 + m / 1024.0) * 2f64.powi(x - 15) };
-            if s == 1 { -v } else { v }
-        };
-        let at = |x: usize, y: usize, z: usize| half(u16::from_le_bytes([v[((z * h + y) * w + x) * e], v[((z * h + y) * w + x) * e + 1]]));
-        let (mut diff, mut mag) = (0.0, 0.0);
-        for z in 0..d - 1 {
-            for y in 0..h {
-                for x in 0..w - 1 {
-                    let c = at(x, y, z);
-                    diff += (at(x + 1, y, z) - c).abs() + (at(x, y, z + 1) - c).abs();
-                    mag += c.abs();
-                }
-            }
-        }
-        diff / mag.max(1e-9)
-    }
-
-    /// (element log2, pixels per side, uncompressed) of the DXGI formats the archive uses
-    fn ttpf_element(dxgi: u32) -> Option<(u32, u32, bool)> {
-        Some(match dxgi {
-            70..=72 | 79..=81 => (3, 4, false),
-            73..=78 | 82..=84 | 94..=99 => (4, 4, false),
-            10 | 11 => (3, 1, true),
-            27..=29 | 87 | 90 | 91 => (2, 1, true),
-            54 => (1, 1, true),
-            61 | 65 => (0, 1, true),
-            _ => return None,
-        })
-    }
-
-    struct TtpfTexture {
-        index: usize,
-        w: u32,
-        h: u32,
-        dxgi: u32,
-        mode: u32,
-        mips: u32,
-        slices: u32,
-        /// dword 1 bits 21-23: 1 2D, 2 cube, 3 volume (slices = depth), 5 2D array
-        dim: u32,
-        start: usize,
-        end: usize,
-    }
-
-    /// Just enough of a .ttpf archive: its zlib blocks of one unpacked stream and the texture
-    /// table.
-    struct Ttpf {
-        d: Vec<u8>,
-        /// (file offset, position in the stream), by position
-        blocks: Vec<(usize, usize)>,
-        textures: Vec<TtpfTexture>,
-        cache: std::cell::RefCell<HashMap<usize, Option<std::rc::Rc<Vec<u8>>>>>,
-    }
-
-    impl Ttpf {
-        fn open(path: &str) -> Ttpf {
-            let d = std::fs::read(path).unwrap();
-            let u32_at = |o: usize| u32::from_le_bytes(d[o..o + 4].try_into().unwrap());
-            assert_eq!(&d[..4], b"RDS5");
-            let total = u32_at(8) as usize | (u32_at(0xc) as usize & 0xff) << 32;
-            let at = (u32_at(0x10) as usize) << 8;
-            let n = (u32_at(4) as usize).min((at - 0x18) / 8);
-            let mut blocks: Vec<(usize, usize)> = (0..n)
-                .filter_map(|i| {
-                    let v = u64::from_le_bytes(d[0x18 + 8 * i..0x20 + 8 * i].try_into().unwrap());
-                    let pos = ((v >> 24) & 0xff_ffff) | ((v >> 48) & 0xf) << 24;
-                    (v != 0).then_some((((v & 0xff_ffff) << 8) as usize, (pos << 6) as usize))
-                })
-                .collect();
-            blocks.sort_by_key(|b| b.1);
-            let count = u32_at(at + 8) as usize;
-            let mut textures: Vec<TtpfTexture> = (0..count)
-                .map(|i| {
-                    let o = at + 16 + 16 * i;
-                    let (a, b, c) = (u32_at(o), u32_at(o + 4), u32_at(o + 8));
-                    TtpfTexture {
-                        index: i,
-                        w: (a & 0x3fff) + 1,
-                        h: ((a >> 14) & 0x3fff) + 1,
-                        dxgi: (b & 0x3ff) >> 3,
-                        // tile mode bits: 0 = 64 KB tiles (SW_64KB_D), else 4 KB (SW_4KB_S)
-                        mode: if (b >> 24) & 3 == 0 { 10 } else { 5 },
-                        mips: (a >> 28).max(1),
-                        slices: ((b >> 10) & 0x7ff) + 1,
-                        dim: (b >> 21) & 7,
-                        start: (c & 0x1f_ffff) as usize * 4096 + 0x800,
-                        end: total,
-                    }
-                })
-                .collect();
-            let mut by_start: Vec<usize> = (0..count).collect();
-            by_start.sort_by_key(|&i| textures[i].start);
-            for k in 0..count.saturating_sub(1) {
-                textures[by_start[k]].end = textures[by_start[k + 1]].start;
-            }
-            Ttpf { d, blocks, textures, cache: Default::default() }
-        }
-
-        /// bytes [a, b) of the unpacked stream (None: in a block that is not zlib)
-        fn read(&self, a: usize, b: usize) -> Option<Vec<u8>> {
-            let mut out = Vec::with_capacity(b - a);
-            let mut pos = a;
-            while pos < b {
-                let i = self.blocks.partition_point(|bl| bl.1 <= pos).checked_sub(1)?;
-                let data = self.block(i)?;
-                let from = pos - self.blocks[i].1;
-                let to = (b - self.blocks[i].1).min(data.len());
-                if to <= from {
-                    return None;
-                }
-                out.extend_from_slice(&data[from..to]);
-                pos += to - from;
-            }
-            Some(out)
-        }
-
-        fn block(&self, i: usize) -> Option<std::rc::Rc<Vec<u8>>> {
-            self.cache
-                .borrow_mut()
-                .entry(i)
-                .or_insert_with(|| {
-                    let off = self.blocks[i].0;
-                    let next = self.blocks.iter().map(|b| b.0).filter(|&o| o > off).min().unwrap_or(self.d.len());
-                    let src = self.d.get(off..next)?;
-                    (src.first() == Some(&0x78)).then(|| miniz_oxide::inflate::decompress_to_vec_zlib(src).ok().map(std::rc::Rc::new)).flatten()
-                })
-                .clone()
-        }
-    }
 }
```

## src/runtime/modules/d3d12x/title_textures.cpp (the C++ port of the design)

Implements `src/hle/d3d12x/title_textures.rs`'s design for the runtime's D3D12X; the
differences, each forced by what the runtime has:

- **Write detection.** XCL write-watches its UMA and graphics reservations (it allocates
  them with `MEM_WRITE_WATCH`). Here only the scratch memory of a Map'd texture is ours to
  allocate, so only that is write-watched (`GetWriteWatch` / `ResetWriteWatch`, as XCL).
  Placed textures lie in the title's physical memory, which the runtime maps as section
  views (`memory.cpp`) — write-watch is impossible there, so those use the page-protection
  and vectored-exception-handler mechanism `placement.cpp` already uses for the same
  memory: read-only between uploads, the first write marks dirty and restores write access.
- **Upload queue.** XCL uploads on its own queue per device and waits on a fence. Here the
  uploads go on the title's own direct queue before its lists (as `upload_placements_d3d12`
  does), one list and one fence wait per refresh; no second queue is made.
- **No sparse tiles, no UMA.** `sparse::before_execute`, `uma`, `kernelx` reservations and
  the frame-trace probes of XCL's design have no counterpart here and are left out; the
  pending / dirty / gpu-written bookkeeping, the batching before ExecuteCommandLists, the
  zero-fill of unreadable memory and the state-keeping barriers are kept.
- **Lifetime.** XCL forgets a texture when the title releases it; here the resource's
  Release is wrapped (only the title's calls, as the other console methods) and forgets at
  the last title reference, freeing the scratch memory with it.
- **Detiling.** XCL detiles with `xg::Texture` + `Chain::detile_level` in process; here the
  same code runs in `xcl_tiling.dll` (the crate above) behind the C ABI, loaded lazily;
  without the DLL every path keeps today's behavior.
