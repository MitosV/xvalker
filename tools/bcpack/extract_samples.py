#!/usr/bin/env python3
"""Extracts BCPack chunks from a texture pool, with Leonida's tools.

Reads an RDS5 texture pool (`tools/ttpf_mips.py` of the Leonida project),
writes every BCPack chunk it holds into a folder, and decodes each of them
with the reference decoder (`tools/bcpack.py` of the same project) so that
the result can be compared with ours, byte for byte.

    python3 tools/bcpack/extract_samples.py <pool.ttpf> [<pool.ttpf> ...]
                                          [--out ~/.cache/xvalker/bcpack-samples]
                                          [--limit N] [--per-mode N]

Every chunk becomes a pair of files in the output folder:

    <pool>-<index>-<codec>.bcpack   the packed chunk
    <pool>-<index>-<codec>.expected  what the reference decoder makes of it

and a line per chunk in `manifest.txt`:

    <file> <codec> <packed bytes> <decoded bytes> <result>

with the result one of `decoded` (the reference decoder made the blocks),
`refused` (it refused the chunk: a history entry it does not confirm) or
`error` (the chunk does not decode at all).

The samples are the game's data: they are written to the cache folder, never
to the repository.
"""
from __future__ import annotations

import argparse
import os
import sys

DEFAULT_REFERENCE = os.path.expanduser("~/dev/projects/ref/Leonida/tools")
DEFAULT_OUT = os.path.expanduser("~/.cache/xvalker/bcpack-samples")
# the codecs the console's DirectStorage uses for its textures
CODECS = {1: "BC1", 3: "BC3", 4: "BC4", 5: "BC5", 6: "BC6H", 7: "BC7"}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pools", nargs="+")
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--reference", default=DEFAULT_REFERENCE,
                        help="the folder with Leonida's tools")
    parser.add_argument("--limit", type=int, default=0,
                        help="at most this many chunks per pool (0: all)")
    parser.add_argument("--per-mode", type=int, default=0,
                        help="at most this many chunks per codec (0: all)")
    args = parser.parse_args(argv)

    sys.path.insert(0, args.reference)
    try:
        import bcpack  # noqa: E402
        import ttpf_mips  # noqa: E402
    except ImportError as error:
        print(f"ERROR: the reference tools are not in {args.reference}: {error}",
              file=sys.stderr)
        return 2

    os.makedirs(args.out, exist_ok=True)
    manifest = os.path.join(args.out, "manifest.txt")
    counts: dict[int, dict[str, int]] = {}
    written = 0
    with open(manifest, "a", encoding="utf-8") as lines:
        for path in args.pools:
            pool = ttpf_mips.Pool(path)
            name = os.path.splitext(os.path.basename(path))[0]
            per_mode: dict[int, int] = {}
            try:
                for index, chunk in enumerate(pool.chunks):
                    virt, size, src, packed, codec, pad = chunk
                    if codec not in CODECS:
                        continue
                    if args.per_mode and per_mode.get(codec, 0) >= args.per_mode:
                        continue
                    if args.limit and per_mode.get(codec, 0) >= args.limit:
                        continue
                    raw = bytes(pool.data[src:src + packed])
                    base = f"{name}-{index}-{CODECS[codec]}"
                    with open(os.path.join(args.out, base + ".bcpack"), "wb") as f:
                        f.write(raw)
                    result = "decoded"
                    try:
                        blocks = bcpack.decode(raw, codec, size)
                    except bcpack.BcpackProvisionalError:
                        result = "refused"
                    except (bcpack.BcpackError, ValueError):
                        result = "error"
                    else:
                        with open(os.path.join(args.out, base + ".expected"), "wb") as f:
                            f.write(blocks)
                    lines.write(f"{base} {codec} {packed} {size} {result}\n")
                    per_mode[codec] = per_mode.get(codec, 0) + 1
                    counts.setdefault(codec, {}).setdefault(result, 0)
                    counts[codec][result] += 1
                    written += 1
            finally:
                pool.close()
            print(f"{path}: {per_mode}", file=sys.stderr)
    print(f"{written} chunks in {args.out}", file=sys.stderr)
    for codec in sorted(counts):
        print(f"  {CODECS[codec]}: {counts[codec]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
