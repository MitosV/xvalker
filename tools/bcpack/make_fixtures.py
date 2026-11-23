#!/usr/bin/env python3
"""Makes the BCPack test vectors, with Leonida's encoder.

The reference project has an encoder for synthetic BCPack chunks (in its
tests): it builds chunks that exercise the block rebuilders of BC1, BC3, BC4
and BC5, which the game's own corpus does not reach in strict mode (their
built-in history entries are not confirmed there). This script writes the
chunks, and what the reference decoder makes of them, into
`tools/bcpack/tests/data/`.

    python3 tools/bcpack/make_fixtures.py [--reference ~/dev/projects/ref/Leonida]

The vectors are small and are part of the repository; the game's own chunks
are not (see `extract_samples.py`).
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

DEFAULT_REFERENCE = os.path.expanduser("~/dev/projects/ref/Leonida")
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "tests", "data")


def load(reference: str):
    sys.path.insert(0, os.path.join(reference, "tools"))
    sys.path.insert(0, os.path.join(reference, "tests"))
    import bcpack  # noqa: E402
    import bcpack_tables as tables  # noqa: E402
    import test_bcpack as ref  # noqa: E402

    # the reference encoder strips the trailing zero bytes of a fragment, and its
    # decoder puts them back as virtual zeros. The console's streams keep them,
    # so the vectors are made without the stripping: both decoders read them.
    fragment = ref._encode_fragment

    def keep_zeros(syms, default_w, w, q, *, flush=True, strip_zeros=False):
        return fragment(syms, default_w, w, q, flush=flush, strip_zeros=strip_zeros)

    ref._encode_fragment = keep_zeros
    return bcpack, tables, ref


def bc1(bcpack, tables, ref):
    """Three blocks: one with no history, one that refers to it, one that
    refers to a confirmed built-in entry."""
    blocks = b"".join([
        struct.pack("<HHBBBB", 0xF800, 0x07E0, 0x12, 0x34, 0x56, 0x78),
        struct.pack("<HHBBBB", 0xF81F, 0x001F, 0xAA, 0xBB, 0xCC, 0xDD),
        struct.pack("<HHBBBB", 0xFFFF, 0x0000, 0x00, 0x55, 0xAA, 0xFF),
    ])
    return ref.encode_bc1_blocks(blocks, ref_dists=[0, 1, 3]), blocks


def bc3(bcpack, tables, ref):
    """One block with no history: the alpha, the colours and the quadrants."""
    inv_pair = {p: i for i, p in enumerate(tables.BC3_ALPHA_PAIR)}
    inv_quad = {v: i for i, v in enumerate(tables.BC3_COLOUR_QUADRANT)}
    a0, a1 = 135, 120
    abits = 0x0123456789AB
    c0 = (5 << 11) | (10 << 5) | 3
    c1 = (2 << 11) | (6 << 5) | 1
    r0, r1, r2, r3 = 0x12, 0x34, 0x56, 0x78
    expected = (bytes([a0, a1]) + abits.to_bytes(6, "little") +
                struct.pack("<HHBBBB", c0, c1, r0, r1, r2, r3))
    lanes = [[] for _ in range(12)]
    lanes[0].extend([0, 0])
    lanes[1].extend([0, 0])
    lanes[2].extend([ref._zig(a0 - 128), ref._zig(a1 - 128)])
    for r in range(4):
        row = (abits >> (12 * r)) & 0xFFF
        lanes[3 + r].extend([
            inv_pair[(row & 7, (row >> 3) & 7)],
            inv_pair[((row >> 6) & 7, (row >> 9) & 7)],
        ])
    lanes[7].extend([ref._zig(5), ref._zig(2)])
    lanes[8].extend([ref._zig(10), ref._zig(6)])
    lanes[9].extend([ref._zig(3), ref._zig(1)])
    qa = (r0 & 15) | ((r1 & 15) << 4)
    qb = (r0 >> 4) | ((r1 >> 4) << 4)
    qc = (r2 & 15) | ((r3 & 15) << 4)
    qd = (r2 >> 4) | ((r3 >> 4) << 4)
    lanes[10].extend([inv_quad[qa], inv_quad[qb]])
    lanes[11].extend([inv_quad[qc], inv_quad[qd]])
    return ref.encode_lanes(lanes, 3), expected


def bc4(bcpack, tables, ref):
    """Two blocks: one with no history, one that refers to it."""
    inv_pair = {p: i for i, p in enumerate(tables.BC4_PAIR)}
    b0_bits = sum(((r + p) & 7) << (3 * (4 * r + p)) for r in range(4) for p in range(4))
    b1_bits = sum(((7 - r - p) & 7) << (3 * (4 * r + p)) for r in range(4) for p in range(4))
    expected = (bytes([130, 120]) + b0_bits.to_bytes(6, "little") +
                bytes([140, 110]) + b1_bits.to_bytes(6, "little"))
    lanes = [[] for _ in range(12)]
    for d, base, (a0, a1), bits in [
        (0, (128, 128), (130, 120), b0_bits),
        (1, (130, 120), (140, 110), b1_bits),
    ]:
        lanes[0].append(ref._zig(d))
        lanes[1].append(0)
        lanes[2].append(ref._zig(a0 - base[0]))
        lanes[3].append(ref._zig(a1 - base[1]))
        for r in range(4):
            row = (bits >> (12 * r)) & 0xFFF
            lanes[4 + 2 * r].append(inv_pair[(row & 7, (row >> 3) & 7)])
            lanes[4 + 2 * r + 1].append(inv_pair[((row >> 6) & 7, (row >> 9) & 7)])
    return ref.encode_lanes(lanes, 4), expected


def bc5(bcpack, tables, ref):
    """Two blocks: one that uses a confirmed built-in entry, one that refers
    to it."""
    inv_pair = {p: i for i, p in enumerate(tables.BC5_PAIR)}
    confirmed_entry = next(iter(sorted(tables.BC5_HISTORY_CONFIRMED)))
    base0 = tables.BC5_HISTORY_CONFIRMED[confirmed_entry]
    d0 = 256 - confirmed_entry
    e0 = ((base0[0] + 2) & 255, (base0[1] - 3) & 255, (base0[2] + 1) & 255,
          (base0[3] - 1) & 255)
    e1 = ((e0[0] + 5) & 255, (e0[1] - 4) & 255, (e0[2] + 3) & 255, (e0[3] + 2) & 255)
    bits = [(0x0123456789AB, 0xFEDCBA987654), (0x112233445566, 0x778899AABBCC)]
    expected = bytearray()
    for e, (br, bg) in zip((e0, e1), bits):
        expected += bytes([e[0], e[1]]) + br.to_bytes(6, "little")
        expected += bytes([e[2], e[3]]) + bg.to_bytes(6, "little")
    lanes = [[] for _ in range(12)]
    for d, base, e, (br, bg) in ((d0, base0, e0, bits[0]), (1, e0, e1, bits[1])):
        sd = d if d < 128 else d - 256
        da, db = ref._deint4(ref._zig(sd))
        lanes[0].extend([da, db])
        lanes[1].extend([0, 0])
        for lane_idx, (c_a, c_b, b_a, b_b) in ((2, (e[0], e[1], base[0], base[1])),
                                               (3, (e[2], e[3], base[2], base[3]))):
            d_a = (c_a - b_a + 128) % 256 - 128
            d_b = (c_b - b_b + 128) % 256 - 128
            lanes[lane_idx].extend([ref._zig(d_a), ref._zig(d_b)])
        for ch, bval in enumerate((br, bg)):
            for r in range(4):
                row = (bval >> (12 * r)) & 0xFFF
                lanes[4 + 4 * ch + r].extend([
                    inv_pair[(row & 7, (row >> 3) & 7)],
                    inv_pair[((row >> 6) & 7, (row >> 9) & 7)],
                ])
    return ref.encode_lanes(lanes, 5), bytes(expected)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--reference", default=DEFAULT_REFERENCE)
    args = parser.parse_args(argv)
    bcpack, tables, ref = load(args.reference)
    os.makedirs(DATA, exist_ok=True)
    for name, maker in (("bc1", bc1), ("bc3", bc3), ("bc4", bc4), ("bc5", bc5)):
        chunk, expected = maker(bcpack, tables, ref)
        # what the reference decoder makes of it, in strict mode
        blocks, provisional = bcpack.decode_with_info(chunk, int(name[2:]), len(expected))
        assert blocks == expected, name
        assert not provisional, (name, provisional)
        with open(os.path.join(DATA, f"{name}.bcpack"), "wb") as f:
            f.write(chunk)
        with open(os.path.join(DATA, f"{name}.expected"), "wb") as f:
            f.write(blocks)
        print(f"{name}: {len(chunk)} bytes, {len(blocks)} bytes of blocks", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
