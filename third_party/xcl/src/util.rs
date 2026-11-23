//! Small helpers shared by all modules: strings, HRESULTs, GDK-style string outputs.

use std::ffi::c_void;

pub type HRESULT = i32;

pub const S_OK: HRESULT = 0;
pub const S_FALSE: HRESULT = 1;
pub const E_NOTIMPL: HRESULT = 0x8000_4001u32 as i32;
pub const E_NOINTERFACE: HRESULT = 0x8000_4002u32 as i32;
pub const E_POINTER: HRESULT = 0x8000_4003u32 as i32;
pub const E_ABORT: HRESULT = 0x8000_4004u32 as i32;
pub const E_FAIL: HRESULT = 0x8000_4005u32 as i32;
pub const E_PENDING: HRESULT = 0x8000_000Au32 as i32;
pub const E_INVALIDARG: HRESULT = 0x8007_0057u32 as i32;
pub const E_OUTOFMEMORY: HRESULT = 0x8007_000Eu32 as i32;
pub const E_NOT_SUFFICIENT_BUFFER: HRESULT = 0x8007_007Au32 as i32;
pub const E_NOT_FOUND: HRESULT = 0x8007_0490u32 as i32;
pub const E_NOT_SUPPORTED: HRESULT = 0x8007_0032u32 as i32;

pub fn hresult_from_win32(code: u32) -> HRESULT {
    if code == 0 {
        0
    } else {
        ((code & 0xFFFF) | 0x8007_0000) as i32
    }
}

/// Parse a return value spec used by the stub DB and config: "E_NOTIMPL", "S_OK",
/// "0x80004001", "-1", "42".
pub fn parse_ret(s: &str) -> Option<u64> {
    let t = s.trim();
    let named = match t.to_ascii_uppercase().as_str() {
        "S_OK" | "0" | "FALSE" | "NULL" => Some(0u64),
        "S_FALSE" | "TRUE" => Some(1),
        "E_NOTIMPL" => Some(E_NOTIMPL as u32 as u64),
        "E_NOINTERFACE" => Some(E_NOINTERFACE as u32 as u64),
        "E_FAIL" => Some(E_FAIL as u32 as u64),
        "E_ABORT" => Some(E_ABORT as u32 as u64),
        "E_INVALIDARG" => Some(E_INVALIDARG as u32 as u64),
        "E_NOT_SUPPORTED" => Some(E_NOT_SUPPORTED as u32 as u64),
        _ => None,
    };
    if named.is_some() {
        return named;
    }
    if let Some(h) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        return u64::from_str_radix(h, 16).ok();
    }
    if let Some(n) = t.strip_prefix('-') {
        return n.parse::<u64>().ok().map(|v| (v as i64).wrapping_neg() as u64);
    }
    t.parse::<u64>().ok()
}

/// UTF-16, NUL-terminated.
pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

pub unsafe fn wcslen(p: *const u16) -> usize {
    let mut n = 0;
    while *p.add(n) != 0 {
        n += 1;
    }
    n
}

pub unsafe fn from_wide_ptr(p: *const u16) -> Option<String> {
    if p.is_null() {
        return None;
    }
    let n = wcslen(p);
    Some(String::from_utf16_lossy(std::slice::from_raw_parts(p, n)))
}

pub unsafe fn from_cstr_ptr(p: *const u8) -> Option<String> {
    if p.is_null() {
        return None;
    }
    Some(
        std::ffi::CStr::from_ptr(p as *const std::ffi::c_char)
            .to_string_lossy()
            .into_owned(),
    )
}

/// ANSI (current code page) → String. Falls back to lossy UTF-8.
pub unsafe fn from_ansi_ptr(p: *const u8) -> Option<String> {
    #[cfg(windows)]
    {
        use windows_sys::Win32::Globalization::{MultiByteToWideChar, CP_ACP};
        if p.is_null() {
            return None;
        }
        let len = std::ffi::CStr::from_ptr(p as *const std::ffi::c_char).to_bytes().len();
        if len == 0 {
            return Some(String::new());
        }
        let n = MultiByteToWideChar(CP_ACP, 0, p, len as i32, std::ptr::null_mut(), 0);
        if n <= 0 {
            return from_cstr_ptr(p);
        }
        let mut buf = vec![0u16; n as usize];
        MultiByteToWideChar(CP_ACP, 0, p, len as i32, buf.as_mut_ptr(), n);
        Some(String::from_utf16_lossy(&buf))
    }
    #[cfg(not(windows))]
    {
        from_cstr_ptr(p)
    }
}

/// Normalised DLL key: lower-case file name, ".dll" appended when no extension.
pub fn dll_key(name: &str) -> String {
    let base = name.rsplit(['\\', '/']).next().unwrap_or(name);
    let mut s = base.trim().to_ascii_lowercase();
    if !s.contains('.') {
        s.push_str(".dll");
    }
    s
}

/// Simple glob: only a single trailing or embedded '*' is supported ("api-ms-win-core-*").
pub fn glob_match(pattern: &str, s: &str) -> bool {
    match pattern.split_once('*') {
        None => pattern == s,
        Some((pre, post)) => s.len() >= pre.len() + post.len() && s.starts_with(pre) && s.ends_with(post),
    }
}

/// GDK-style "copy string into caller buffer" helper: `size` includes the terminator.
pub unsafe fn write_cstr_out(src: &str, buf: *mut u8, size: usize, used: *mut usize) -> HRESULT {
    let needed = src.len() + 1;
    if !used.is_null() {
        *used = needed;
    }
    if buf.is_null() || size < needed {
        return E_NOT_SUFFICIENT_BUFFER;
    }
    std::ptr::copy_nonoverlapping(src.as_ptr(), buf, src.len());
    *buf.add(src.len()) = 0;
    S_OK
}

/// Win32-style "copy wide string" (GetModuleFileNameW semantics): returns chars written
/// (without terminator) or `size` when truncated.
pub unsafe fn write_wide_out(src: &str, buf: *mut u16, size: u32) -> u32 {
    if buf.is_null() || size == 0 {
        return 0;
    }
    let w: Vec<u16> = src.encode_utf16().collect();
    let n = w.len().min(size as usize - 1);
    std::ptr::copy_nonoverlapping(w.as_ptr(), buf, n);
    *buf.add(n) = 0;
    if w.len() >= size as usize {
        #[cfg(windows)]
        windows_sys::Win32::Foundation::SetLastError(122); // ERROR_INSUFFICIENT_BUFFER
        size
    } else {
        n as u32
    }
}

pub unsafe fn write_ansi_out(src: &str, buf: *mut u8, size: u32) -> u32 {
    if buf.is_null() || size == 0 {
        return 0;
    }
    let b = src.as_bytes();
    let n = b.len().min(size as usize - 1);
    std::ptr::copy_nonoverlapping(b.as_ptr(), buf, n);
    *buf.add(n) = 0;
    if b.len() >= size as usize {
        #[cfg(windows)]
        windows_sys::Win32::Foundation::SetLastError(122);
        size
    } else {
        n as u32
    }
}

/// Wrapper to keep raw pointers in statics.
#[derive(Clone, Copy, Debug)]
pub struct SendPtr(pub *mut c_void);
unsafe impl Send for SendPtr {}
unsafe impl Sync for SendPtr {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dll_keys() {
        assert_eq!(dll_key("KERNELX"), "kernelx.dll");
        assert_eq!(dll_key("G:\\Bin\\Foo.DLL"), "foo.dll");
        assert_eq!(dll_key("api-ms-win-core-file-l1-1-0.dll"), "api-ms-win-core-file-l1-1-0.dll");
    }

    #[test]
    fn globbing() {
        assert!(glob_match("api-ms-win-core-*", "api-ms-win-core-file-l1-1-0.dll"));
        assert!(!glob_match("api-ms-win-core-*", "api-ms-win-crt-runtime-l1-1-0.dll"));
        assert!(glob_match("xgameruntime*.dll", "xgameruntime.thunks.dll"));
        assert!(glob_match("kernelx.dll", "kernelx.dll"));
    }

    #[test]
    fn return_specs() {
        assert_eq!(parse_ret("E_NOTIMPL"), Some(0x8000_4001));
        assert_eq!(parse_ret("0x10"), Some(16));
        assert_eq!(parse_ret("-1"), Some(u64::MAX));
        assert_eq!(parse_ret("7"), Some(7));
        assert_eq!(parse_ret("bogus"), None);
    }

    #[test]
    fn cstr_out() {
        let mut buf = [0u8; 8];
        let mut used = 0usize;
        unsafe {
            assert_eq!(write_cstr_out("abc", buf.as_mut_ptr(), 8, &mut used), S_OK);
            assert_eq!(used, 4);
            assert_eq!(&buf[..4], b"abc\0");
            assert_eq!(write_cstr_out("toolongstr", buf.as_mut_ptr(), 8, &mut used), E_NOT_SUFFICIENT_BUFFER);
            assert_eq!(used, 11);
        }
    }
}
