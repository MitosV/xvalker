# Edits to the vendored XCL files

Every edit of the vendored files of XCL `dd713e3` is listed here. The goal of the
edits is one thing only: **let the files build outside XCL, on Linux**, without
changing what the recompiler does.

The files not listed here are byte-identical to their originals.

---

## `src/gpu/recomp/mod.rs`

The module declared three Windows-only children and a test module.

Removed (5 lines):

```
-pub mod dxc;
-pub mod pso;
-pub mod runner;
-#[cfg(test)]
-mod tests;
```

`dxc` (the `dxcompiler.dll` COM interface), `pso` and `runner` (D3D12) are Windows
only. `dxc` has a Linux counterpart in `tools/xcl-recomp/src/dxc.rs` (the `dxc` CLI);
`pso` and `runner` are the runtime's job, not the translator's.

The remaining children (`emit`, `mesh_dispatch`, `mesh_wave`) are unchanged and are
found next to `mod.rs` in `third_party/xcl/src/gpu/recomp/`.

---

## `src/shaders.rs`

`shaders.rs` calls the DXC module of the recompiler. On Linux that module is the
crate's own `dxc` module, so the path changed (4 call sites + 1 comment):

```
-`recomp::dxc::cache_dir()`
+`crate::dxc::cache_dir()`
-recomp::dxc::compile_cached(
+crate::dxc::compile_cached(
-recomp::dxc::compile(
+crate::dxc::compile(   (2 sites)
```

---

## `src/gpu/shader.rs`

The runtime capture part of the module (the D3D12 hooks that dump the shaders a
title submits) is Windows only. It is now behind `#[cfg(windows)]`, so the file
builds on Linux; the parts the recompiler needs (`container_parts`, `classify`,
`Kind`, `fnv64`, `rdna2_listing`) are untouched.

Added `#[cfg(windows)]` (10 lines):

```
 struct State {                    ->  #[cfg(windows)]
 static STATE: Mutex<Option<State>>  ->  #[cfg(windows)]
 fn dump_dir() -> PathBuf {       ->  #[cfg(windows)]
 pub fn readable(p: usize, ...)   ->  #[cfg(windows)]
 pub fn capture(stage: &str, ...) ->  #[cfg(windows)]
 fn capture_xbox(blob: &[u8], ..) ->  #[cfg(windows)]
 fn recompile(blob: &[u8], ...)   ->  #[cfg(windows)]
 pub fn stats() -> ShaderStats    ->  #[cfg(windows)]
```

The imports that only the Windows part uses are now conditional (3 lines):

```
-use std::collections::{BTreeMap, HashSet};
-use std::path::PathBuf;
-use std::sync::Mutex;
+use std::collections::BTreeMap;
+#[cfg(windows)]
+use std::collections::HashSet;
+#[cfg(windows)]
+use std::path::PathBuf;
+#[cfg(windows)]
+use std::sync::Mutex;
```

The test module of the file needs `readable`, which is Windows only, so it is behind
the `xcl_vendored_tests` feature of the crate (1 line):

```
-#[cfg(test)]
+#[cfg(all(test, feature = "xcl_vendored_tests"))]
 mod tests {
```

One call site changed, like in `shaders.rs` (1 line):

```
-            let built = if dxc && t.complete() { Some(recomp::dxc::compile_cached(&t.hlsl, "main", profile)) } else { None };
+            let built = if dxc && t.complete() { Some(crate::dxc::compile_cached(&t.hlsl, "main", profile)) } else { None };
```

---

## `src/gpu/rdna2.rs`

The test module of the file reads the LLVM disassembly corpus of XCL
(`tests/data/llvm-amdgpu`), which is not part of the vendored files. The test module
is behind the `xcl_vendored_tests` feature of the crate (1 line):

```
-#[cfg(test)]
+#[cfg(all(test, feature = "xcl_vendored_tests"))]
 mod tests {
```

---

## `src/gpu/recomp/mesh_dispatch.rs`

The test module of the file needs XCL's Windows-only modules `recomp::dxc` and
`recomp::runner`, which are not vendored. The test module is behind the
`xcl_vendored_tests` feature of the crate (1 line):

```
-#[cfg(test)]
+#[cfg(all(test, feature = "xcl_vendored_tests"))]
 mod tests {
```

---

## `src/rpf.rs`

The atomic extraction wrote the file with `MoveFileExW` (Windows only).

The import of `std::os::windows::ffi::OsStrExt` and of `windows_sys` was removed
(3 lines):

```
-use std::os::windows::ffi::OsStrExt;
-use windows_sys::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_WRITE_THROUGH};
```

The commit of the file is now platform specific (the Windows branch keeps XCL's
code, the Linux branch renames the temporary file without replacing an existing
one, as Windows' `MoveFileExW` does without `MOVEFILE_REPLACE_EXISTING`):

```
-    let from: Vec<u16> = temporary.0.as_os_str().encode_wide().chain([0]).collect();
-    let to: Vec<u16> = path.as_os_str().encode_wide().chain([0]).collect();
-    if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), MOVEFILE_WRITE_THROUGH) } == 0 {
-        return Err(io::Error::last_os_error());
+    #[cfg(windows)]
+    {
+        use std::os::windows::ffi::OsStrExt;
+        use windows_sys::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_WRITE_THROUGH};
+        let from: Vec<u16> = temporary.0.as_os_str().encode_wide().chain([0]).collect();
+        let to: Vec<u16> = path.as_os_str().encode_wide().chain([0]).collect();
+        if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), MOVEFILE_WRITE_THROUGH) } == 0 {
+            return Err(io::Error::last_os_error());
+        }
     }
+    #[cfg(not(windows))]
+    rename_noreplace(&temporary.0, path)?;
```

`rename_noreplace` is new: `std::fs::rename` replaces the target, but the extraction
must not clobber a file that appeared meanwhile. Linux has `renameat2` with
`RENAME_NOREPLACE` for that (the crate depends on `libc` for it on non-Windows).

---

## `src/util.rs`

`util.rs` uses `windows_sys` in three places. The Windows code is kept behind
`#[cfg(windows)]`; on Linux the ANSI conversion falls back to the bytes as they are
(there is no code page).

`from_ansi_ptr` (the body is now platform specific, 17 lines):

```
-pub unsafe fn from_ansi_ptr(p: *const u8) -> Option<String> {
-    use windows_sys::Win32::Globalization::{MultiByteToWideChar, CP_ACP};
-    ...
+pub unsafe fn from_ansi_ptr(p: *const u8) -> Option<String> {
+    #[cfg(windows)]
+    {
+        use windows_sys::Win32::Globalization::{MultiByteToWideChar, CP_ACP};
+        ...
+    }
+    #[cfg(not(windows))]
+    {
+        from_cstr_ptr(p)
+    }
```

`SetLastError` after a truncated copy (2 lines):

```
-        windows_sys::Win32::Foundation::SetLastError(122); // ERROR_INSUFFICIENT_BUFFER
+        #[cfg(windows)]
+        windows_sys::Win32::Foundation::SetLastError(122); // ERROR_INSUFFICIENT_BUFFER
```

```
-        windows_sys::Win32::Foundation::SetLastError(122);
+        #[cfg(windows)]
+        windows_sys::Win32::Foundation::SetLastError(122);
```

## `src/gpu/recomp/mod.rs` (wave64 on a 32-lane host)

The corpus tool models an NVIDIA GPU (host subgroup 32), so wave64 programs must say
which cross-half operations their host cannot perform. Added to the vendored emitter:

- `Options.host_wave: u32` (0 = native wave64, as before; 32 = two native wave32 halves).
- `host_hazards(code, mode, host_wave)`: `geometry_wave32_hazards` for mesh/geometry (the split
  forms also carry a mask), `wave_hazards` for the other stages.
- `translate` refuses a wave64 program when `host_hazards` is not empty, before emitting HLSL.
- `translate_geometry` / `translate_mesh` pass `host_wave = 32` when they split the wave.

No other vendored file is changed by these edits.

## xvalker build of the DLL (orchestrator)

`src/gpu/shader.rs`: the 11 `#[cfg(windows)]` gates added above are `#[cfg(xcl_runtime)]`
(a cfg that is never set), so the Windows DLL does not build XCL's runtime capture code.
`src/rpf.rs` is unchanged; its Windows branch uses `windows-sys`, a Windows-only dependency of
`tools/xcl-recomp`.
## `src/gpu/recomp/wave64_host.rs` and emit/hazard changes (idempotent reductions)

A 32-lane host cannot read a wave64's lane `32+k`. A VGPR whose value is a min/max/and/or
reduction over DPP/permlanex16 is reduced in each half separately; `v_readlane_b32 ..., 32+k`
is emitted as `WaveReadLaneAt(v, k & 31)` (`rla_local`). The hazard detectors skip those
reads and the recognized DPP/permlane chain. `add`/`xor` are not classified as idempotent.

## Uniform readlane on a 32-lane host

A `v_readlane_b32` whose source VGPR is proven uniform over the wave (SGPR broadcast or
folded reduction) is emitted as the invoking lane's own value. SGPRs are seeded as uniform
except saved-EXEC registers. The corresponding readlane is no longer refused.

## Groupshared cross-half exchange on a 32-lane host

Compute/mesh/geometry run one logical wave64 as two 32-lane subgroups in one workgroup.
`ds_bpermute`, `v_readlane` and `v_writelane` of the other half publish values in
`groupshared uint xcl_xchg[64]` and exchange them with two group barriers. VS/PS have no
workgroup and still refuse these reads (one invocation per fragment/vertex).

## `src/gpu/recomp/emit.rs` (mesh pass-through vertex stride)

`passthrough_split` budgeted the per-vertex output as `16 * (1 + elements)`, but DXC's
mesh validator pads the stride to a power of two of 16-byte elements (SV_Position
included): 21 outputs cost 32 slots = 512 bytes per vertex. With 78 vertices that
exceeded the 32 KB group limit and DXC reported "vertex plus primitive output size is
greater than 32768" (19 MS programs). Round the stride the same way before deciding
how many groups a mesh record needs.

## `src/gpu/recomp/emit.rs` (full wave64 masks on a 32-lane host)

A split logical wave64 runs its two halves in separate native wave32 subgroups, so a
`v_cmp` ballot only sees 32 lanes. Each half now publishes its ballot word to two
groupshared slots per logical wave, barrier, and both halves read the combined 64-bit
mask. SGPRs derived from a mask (EXEC/VCC, `v_cmp`, `s_and/or/xor_b64`) are therefore
identical in both halves, as are the `scc`, `execz`/`vccz` branch decisions and
`s_bcnt1`/`s_ff1` reductions computed from them. The exchange array reserves 66 dwords
per logical wave (64 lane slots + 2 ballot words).

## `src/gpu/recomp/emit.rs` (groupshared budget with cross-half exchange)

The groupshared exchange added 64–1024 dwords on top of the emitter's full 32 KB
`xcl_lds`, so DXC rejected 100 compute programs with "Total Thread Group Shared Memory
used ... is 33024, exceeding maximum: 32768". Reserve 64 dwords per logical wave64 in
the workgroup for `xcl_xchg`, size `xcl_lds` from the same 8192-dword budget, and use
the matching index mask for both. This also keeps multi-wave workgroups from indexing
past the exchange array.
