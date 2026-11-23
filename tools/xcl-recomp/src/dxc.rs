//! DXC on Linux: the `dxc` command line tool of the DirectXShaderCompiler release.
//!
//! XCL compiles on Windows with the `dxcompiler.dll` COM interface (`gpu/recomp/dxc.rs`).
//! On Linux the same job is done by the `dxc` CLI of the official release; this module
//! is the Linux counterpart of XCL's `dxc.rs`, with the same arguments and the same
//! cache directory layout.
//!
//! The arguments are the ones XCL uses (`gpu/recomp/dxc.rs`):
//! `-E main -T <profile> -HV 2021`, plus `-O0` when the HLSL is larger than 8 MiB.

use std::path::{Path, PathBuf};
use std::process::Command;

/// The DXC executable: `XCL_DXC` when set, else `dxc` on the `PATH`.
pub fn dxc_path() -> PathBuf {
    if let Ok(p) = std::env::var("XCL_DXC") {
        return PathBuf::from(p);
    }
    PathBuf::from("dxc")
}

/// The DXIL cache directory: `XCL_DXIL_CACHE` when set, else
/// `~/.cache/xvalker/dxil-cache`.
pub fn cache_dir() -> PathBuf {
    if let Ok(p) = std::env::var("XCL_DXIL_CACHE") {
        return PathBuf::from(p);
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".to_string());
    PathBuf::from(home).join(".cache/xvalker/dxil-cache")
}

/// The cache key of a program: the hash of the profile, the entry point and the HLSL.
pub fn cache_key(src: &str, entry: &str, profile: &str) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    let mut feed = |bytes: &[u8]| {
        for b in bytes {
            h ^= *b as u64;
            h = h.wrapping_mul(0x100_0000_01b3);
        }
    };
    feed(profile.as_bytes());
    feed(b"\0");
    feed(entry.as_bytes());
    feed(b"\0");
    feed(src.as_bytes());
    h
}

/// The arguments XCL passes to DXC for `src`.
pub fn args(src: &str, entry: &str, profile: &str) -> Vec<String> {
    let mut args = vec!["-E".to_string(), entry.to_string(), "-T".to_string(), profile.to_string(), "-HV".to_string(), "2021".to_string()];
    // DXC's optimiser was superlinear on translated programs full of short-circuit
    // expressions; -O0 only as a safety net for enormous sources (XCL's rule).
    if src.len() > 8 * 1024 * 1024 {
        args.push("-O0".to_string());
    }
    args
}

/// Compile `src` to signed DXIL with the Linux `dxc` CLI. `extra` are dxc arguments.
pub fn compile(src: &str, entry: &str, profile: &str, extra: &[&str]) -> Result<Vec<u8>, String> {
    let out = compile_to_file(src, entry, profile, extra)?;
    let data = std::fs::read(&out).map_err(|e| format!("dxc: cannot read {}: {e}", out.display()));
    // The folder is ours; the DXIL was read (or lost).
    if let Some(dir) = out.parent() {
        let _ = std::fs::remove_dir_all(dir);
    }
    data
}

/// Compile `src` into `out` (the DXIL file). `extra` are dxc arguments.
pub fn compile_to_file(src: &str, entry: &str, profile: &str, extra: &[&str]) -> Result<PathBuf, String> {
    // Every compile gets its own folder: the corpus tool runs many at a time.
    static COUNTER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let id = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let dir = std::env::temp_dir().join(format!("xcl-dxc-{}-{id}", std::process::id()));
    std::fs::create_dir_all(&dir).map_err(|e| format!("dxc: cannot create {}: {e}", dir.display()))?;
    let source = dir.join("shader.hlsl");
    let dxil = dir.join("shader.dxil");
    std::fs::write(&source, src).map_err(|e| format!("dxc: cannot write {}: {e}", source.display()))?;
    let mut cmd = Command::new(dxc_path());
    cmd.args(args(src, entry, profile));
    cmd.args(extra);
    cmd.arg("-Fo").arg(&dxil);
    cmd.arg(&source);
    let output = cmd.output().map_err(|e| format!("dxc: cannot run {}: {e}", dxc_path().display()))?;
    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr).into_owned();
    if !output.status.success() {
        let _ = std::fs::remove_dir_all(&dir);
        return Err(format!("dxc: {}{}", stdout, stderr));
    }
    if !dxil.exists() {
        let _ = std::fs::remove_dir_all(&dir);
        return Err(format!("dxc: {}{}", stdout, stderr));
    }
    // The DXIL is read by the caller; keep it until then.
    Ok(dxil)
}

/// Compile `src` into the DXIL cache. The second value of the pair is true when the
/// program was already in the cache (nothing was compiled).
pub fn compile_cached(src: &str, entry: &str, profile: &str) -> Result<(Vec<u8>, bool), String> {
    compile_cached_in(&cache_dir(), src, entry, profile)
}

/// Compile `src` into the DXIL cache of `dir`.
pub fn compile_cached_in(dir: &Path, src: &str, entry: &str, profile: &str) -> Result<(Vec<u8>, bool), String> {
    let key = cache_key(src, entry, profile);
    let path = dir.join(format!("{key:016x}.dxil"));
    if let Ok(data) = std::fs::read(&path) {
        return Ok((data, true));
    }
    let data = compile(src, entry, profile, &[])?;
    if std::fs::create_dir_all(dir).is_ok() {
        let _ = std::fs::write(&path, &data);
    }
    Ok((data, false))
}

/// The toolchain identity (the DXC version), part of the cache key of XCL's Windows
/// implementation. The Linux CLI reports its version with `-version`.
pub fn toolchain_identity() -> String {
    let output = Command::new(dxc_path()).arg("--version").output();
    match output {
        Ok(out) => {
            let mut text = String::from_utf8_lossy(&out.stdout).into_owned();
            text.push_str(&String::from_utf8_lossy(&out.stderr));
            let first = text.lines().find(|l| l.contains("v1.") || l.contains("Version")).unwrap_or("").trim().to_string();
            if first.is_empty() {
                "unknown".to_string()
            } else {
                first
            }
        }
        Err(_) => "unavailable".to_string(),
    }
}
