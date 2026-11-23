# bcpack

Vendored BCPack decoder from the `xbox-emulator` project.

- Source: `~/dev/projects/ref/xbox-emulator`
- Source path: `crates/compat/xcrdapi/src/bcpack/`
- Source commit: `d8d37e4` (`git -C ~/dev/projects/ref/xbox-emulator rev-parse --short HEAD`)
- License: GPL-3.0-or-later for the xcrdapi integration; the rANS and BC1 code
  itself comes from `bcpack-rs` (MIT OR Apache-2.0) and `silicon-rans` (MIT).
  The license texts are kept next to the sources as `LICENSE-*`.

The Rust sources (`bc1.rs`, `encode.rs`, `golden.txt`, `mod.rs`, `rans.rs`,
`tables.rs`, `tests.rs`, `xcrd_tests.rs`) are byte-identical copies of the
upstream module. Do not edit them here: they are wrapped by
`tools/bcpack` (see that crate's `README.md`), which supplies the
`crate::abi`, `crate::request` and `compat_common` shims the module expects.
