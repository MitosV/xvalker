# Edits to the vendored XCL files

Every edit of the vendored files of XCL `db5dc26` is listed here. The goal of the
edits is one thing only: **let the files build outside XCL, on Linux**, without
changing what the recompiler does. The edits that changed what the recompiler
does are the two the upstream did not take (the mesh pass-through's vertex
stride) and the ones upstream took over (the wave64 form on a 32-lane host).

The files not listed here are byte-identical to their originals.

---

## What upstream took over

Upstream `db5dc26` has the work xvalker had done for the wave64 programs on a
32-lane host, and more:

* `gpu/recomp/x64_masks.rs` (new): which masks are read whole (so the two
  halves must exchange them) and which are only read lane by lane (so they stay
  in the half). `gpu/recomp/upper_lanes.rs` (new): what the lanes 32..63 of a
  wave64 VS / PS hold, so the 32-lane form can give the program their values.
* `Options::split_wave64` replaces xvalker's `host_wave` / `geometry_wave32` /
  `mesh_wave32`: a wave64 compute / mesh / geometry program runs as two native
  wave32 halves that exchange their masks and lane values through groupshared
  memory (`docs/wave64-on-wave32.md` of XCL), and a wave64 VS / PS gets the
  values of the lanes >= 32 it reads.
* Upstream no longer refuses a program for the host's wave width: the split
  form has no rejection list. On the corpus, the 364 pixel programs xvalker had
  to refuse are translated now.

So xvalker's own wave64 form (`gpu/recomp/wave64_host.rs` and the emitter
changes that went with it: the idempotent reductions, the uniform readlane, the
groupshared exchange, the full wave64 masks) is **dropped**: upstream's form
covers all of it, and it is upstream's own code (the audited one). The file
`gpu/recomp/wave64_host.rs` is gone.

What upstream does **not** cover, and xvalker keeps:

* the mesh pass-through's vertex stride (below): upstream's `passthrough_split`
  still budgets the per-vertex output as `16 * (1 + elements)`, but DXC's mesh
  validator rounds the stride up to a power of two of 16-byte elements.

Upstream also fixed the MUBUF index bug xvalker had patched (a `MUBUF` load
whose `VDATA` overlaps the index `VADDR`): upstream's load is branch-free now
and computes every bounds check of the instruction in one statement, before it
writes the loaded components into `VDATA`. The xvalker patch (a snapshot of the
index in a local) is therefore **dropped**.

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

The remaining children (`emit`, `mesh_dispatch`, `mesh_wave`, `x64_masks`,
`upper_lanes`) are unchanged and are found next to `mod.rs` in
`third_party/xcl/src/gpu/recomp/`.

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

The `--precompile` path had two parts that need XCL's D3D12 modules, which are
not vendored:

* the split wave64 form's default: the adapter's wave width, which XCL asks its
  own device (`pso::split_wave64_forced`, `runner::wave64_supported`). The
  caller decides now (`--split`); the default is off.

```
-let split = split.unwrap_or_else(|| recomp::pso::split_wave64_forced() || recomp::runner::wave64_supported().is_ok_and(|w| !w));
+let split = split.unwrap_or(false);
```

* the compute programs as a title's pipeline translates them
  (`pso::console_to_dxil_in`). The generic loop below translates them with the
  same options (`windows`, `split`) and compiles them into the same cache, so
  the block is out.

---

## `src/gpu/shader.rs`

The runtime capture part of the module (the D3D12 hooks that dump the shaders a
title submits) is Windows only. It is now behind `#[cfg(xcl_runtime)]`, so the file
builds on Linux; the parts the recompiler needs (`container_parts`, `classify`,
`Kind`, `fnv64`, `rdna2_listing`) are untouched.

Added `#[cfg(xcl_runtime)]` (12 lines):

```
 struct State {                    ->  #[cfg(xcl_runtime)]
 static STATE: Mutex<Option<State>>  ->  #[cfg(xcl_runtime)]
 fn dump_dir() -> PathBuf {       ->  #[cfg(xcl_runtime)]
 pub fn readable(p: usize, ...)   ->  #[cfg(xcl_runtime)]
 pub fn capture(stage: &str, ...) ->  #[cfg(xcl_runtime)]
 fn dumps_enabled() -> bool {     ->  #[cfg(xcl_runtime)]
 fn account_xbox(blob: &[u8], ..) ->  #[cfg(xcl_runtime)]
 fn capture_xbox(blob: &[u8], ..) ->  #[cfg(xcl_runtime)]
 fn recompile(blob: &[u8], ...)   ->  #[cfg(xcl_runtime)]
 pub fn stats() -> ShaderStats    ->  #[cfg(xcl_runtime)]
```

The imports that only the Windows part uses are now conditional (3 lines):

```
-use std::collections::{BTreeMap, HashSet};
-use std::path::PathBuf;
-use std::sync::Mutex;
+use std::collections::BTreeMap;
+#[cfg(xcl_runtime)]
+use std::collections::HashSet;
+#[cfg(xcl_runtime)]
+use std::path::PathBuf;
+#[cfg(xcl_runtime)]
+use std::sync::Mutex;
```

The test module of the file needs `readable`, which is Windows only, so it is behind the
`xcl_vendored_tests` feature of the crate (1 line):

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

## xvalker build of the DLL (orchestrator)

`src/gpu/shader.rs`: the `#[cfg(xcl_runtime)]` gates added above are a cfg that is
never set, so the Windows DLL does not build XCL's runtime capture code.
`src/rpf.rs` keeps its Windows branch; it uses `windows-sys`, a Windows-only
dependency of `tools/xcl-recomp`.

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

Not edited any more: the file is XCL's Windows helper module (`HRESULT`s, the ANSI
string helpers), and no crate of XValkyrie builds it — `tools/xcl-tiling` has its
own small `util` module (the `HRESULT` terms the vendored files use). Upstream's
`db5dc26` file is vendored byte-identical.

---

## `src/gpu/recomp/emit.rs` (mesh pass-through vertex stride)

`passthrough_split` budgeted the per-vertex output as `16 * (1 + elements)`, but
DXC's mesh validator pads the stride to a power of two of 16-byte elements
(SV_Position included): 21 outputs cost 32 slots = 512 bytes per vertex. With 78
vertices that exceeded the 32 KB group limit and DXC reported "vertex plus
primitive output size is greater than 32768" (19 MS programs). Round the stride
the same way before deciding how many groups a mesh record needs.

```
-    let vbytes = 16 * (1 + elements as u32);
+    // DXC's mesh validator rounds the per-vertex stride up to a power of two of
+    // 16-byte output elements (SV_Position included): 17 elements cost 32 slots,
+    // not 17. Check the same 32 KB budget it checks, otherwise the pass-through
+    // fails with "vertex plus primitive output size is greater than 32768".
+    let slots = (1 + elements as u32).next_power_of_two();
+    let vbytes = 16 * slots;
```
