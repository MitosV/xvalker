# bcpack

`bcpack.dll`: the BCPack decoder the runtime's DirectStorage module calls.

- Vendored decoder: `third_party/bcpack/` (the `xbox-emulator` project's
  `xcrdapi` BCPack module, byte for byte; see its README for the licenses).
- `src/bc1_history.rs`, `src/bc1_quad.rs`: the BC1 built-in history and the
  quadrant codes, as the reference decoder (`tools/bcpack.py` of the Leonida
  project, GPL-3.0) has them.
- `src/bc345.rs`: BC3, BC4 and BC5, a Rust port of Leonida's strict
  rebuilders.

## The C ABI

```c
// src/runtime/modules/bcpack.h
int32_t xv_bcpack_decode(uint32_t mode, const uint8_t *src, size_t src_size,
                          uint8_t *dst, size_t dst_size);
```

`mode` is the console's `DSTORAGE_BCPACK_MODE`: 1 = BC1, 3 = BC3, 4 = BC4,
5 = BC5. `dst_size` is the chunk's decoded byte count and must be a whole
number of blocks (8 bytes for BC1 and BC4, 16 for BC3 and BC5). The call
returns `S_OK` (0), `E_DSTORAGE_INVALID_DESTINATION_SIZE` (0x8924000F) or
`E_DSTORAGE_DECOMPRESSION_ERROR` (0x89240030). Nothing is written to the
destination when it fails, and every failure has a line on stderr.

Modes 2 (BC2), 6 (BC6H) and 7 (BC7) are refused: their block layouts are not
confirmed, and the runtime does not guess.

## The tables

The vendored module brings a window of its own for the BC1 built-in history
and its own quadrant codes. The crate uses the reference decoder's tables
instead: they differ in 76 of the 256 history entries and in 4 of the 256
quadrant codes, and the reference measured its own against the game's
textures. The entries the reference does not confirm are absent, so a chunk
that reaches one of them is refused instead of being decoded with guessed
endpoints, the way the reference refuses it.

## Building

```sh
tools/bcpack/build.sh [output-directory]
```

builds `bcpack.dll` for `x86_64-pc-windows-gnu` and copies it next to the
executable (by default `build/linux`, the folder `tools/build-linux.sh` uses).
The DLL is not part of the CMake build; the runtime loads it on the first
compressed request and fails that request when it is not there.

## The tests

```sh
cargo test
```

runs the vendored tests (with their golden hashes), the test vectors in
`tests/data/` (made by `make_fixtures.py` with the reference project's
encoder) and the comparison with the reference decoder over the game's own
chunks.

The game's chunks are not in the repository. `extract_samples.py` writes them,
and what the reference decoder makes of them, into
`~/.cache/xvalker/bcpack-samples`:

```sh
python3 tools/bcpack/extract_samples.py <pool.ttpf> [<pool.ttpf> ...]
```

The comparison test reads that folder (`$XV_BCPACK_SAMPLES` names another
one) and does nothing when it is not there.

`cargo run --example decode -- <mode> <chunk> <size> [out]` decodes one
chunk and writes its blocks.
