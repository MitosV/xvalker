#!/usr/bin/env python3
"""Build one serialized scenario per shader for the xcl-vs-cpu_ref comparison.

The scenario is the single source of truth: run_real.build produces the console
scenario lines, and this script turns them into a compact guest-memory image,
SGPR/VGPR inputs, V# descriptors and XCL T# words. Both the CPU interpreter and
the Rust GPU test load the same JSON file; neither rebuilds its inputs.
"""
import argparse, csv, hashlib, json, os, random, struct, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = Path(os.environ.get("XBOX_TOOLS", "/tmp/opencode/americas-s4/tools/xbox"))
sys.path.insert(0, str(TOOLS))
import run_real as R  # noqa: E402

MASK64 = (1 << 64) - 1
WAVE = 32              # RTX 4070 Ti has no wave64; both sides use 32 lanes
DATA_BYTES = 1 << 20
NODE_BYTES = 1 << 16
IMG = 64
IMG3D = 16
LAYERS = 4
DIM_TYPE = {"1d": 8, "2d": 9, "3d": 10, "cube": 11, "2da": 13}


def fnv(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & MASK64
    return f"{h:016x}"


class Scenario:
    def __init__(self, idx):
        self.idx = str(idx)
        self.regions = {}       # rid -> bytearray
        self.base = {}          # rid -> compact guest address
        self.images = {}        # iid -> metadata
        self.timg = []          # (region, offset, iid)
        self.sgpr = {}
        self.data_regions = []  # rids whose final bytes are compared
        self.code = b""

    def finish(self):
        """Assign compact bases, rewrite all pointers/descriptors, hash inputs."""
        end = 0x10000
        for rid in sorted(self.regions):
            self.base[rid] = end
            end += len(self.regions[rid])
        for reg, off, value in self.pokes:
            data = self.regions[reg]
            if off + 4 <= len(data):
                struct.pack_into("<I", data, off, value & 0xFFFFFFFF)
        for reg, off, target, extra in self.ptrs:
            data = self.regions[reg]
            struct.pack_into("<I", data, off, (self.base[target] + extra) & 0xFFFFFFFF)
        for reg, off, target, extra in self.ptrhi:
            data = self.regions[reg]
            t = self.base[target]
            struct.pack_into("<I", data, off, (((t >> 32) & 0xFFFF) | extra) & 0xFFFFFFFF)
        for reg, off, iid in self.timg:
            im = self.images[iid]
            srv = 2 * (iid - 1)
            words = [srv, im["fmt"] << 20, (im["w"] - 1) | ((im["h"] - 1) << 14),
                     0xFAC | (DIM_TYPE[im["dim"]] << 28),
                     (im["depth"] - 1) if im["dim"] == "3d" else ((im["layers"] - 1) if im["layers"] > 1 else 0),
                     0, 0, 0]
            data = self.regions[reg]
            for k, v in enumerate(words):
                struct.pack_into("<I", data, off + 4 * k, v & 0xFFFFFFFF)
        sg = [0] * 128
        for reg, (kind, value) in self.user.items():
            if 0 <= reg < 128:
                sg[reg] = (self.base[value] if kind == "r" else value) & 0xFFFFFFFF
        sg[126], sg[127] = 0xFFFFFFFF, 0            # exec: 32 active lanes
        vgpr = [[0] * 256 for _ in range(WAVE)]
        for lane in range(WAVE):
            vgpr[lane][0] = lane
        hashes = {"code": fnv(self.code), "sgpr": fnv(b"".join(struct.pack("<I", v) for v in sg)),
                  "vgpr": fnv(b"".join(struct.pack("<I", v) for row in vgpr for v in row))}
        regions = []
        for rid in sorted(self.regions):
            data = bytes(self.regions[rid])
            key = f"region:0x{self.base[rid]:x}"
            hashes[key] = fnv(struct.pack("<II", self.base[rid], len(data)) + data)
            regions.append({"base": self.base[rid], "data": data.hex()})
        images = []
        for iid in sorted(self.images):
            im = self.images[iid]
            key = f"image:{2 * (iid - 1)}"
            hashes[key] = fnv(im["data"])
            images.append({"id": iid, "srv_index": 2 * (iid - 1), "dim": DIM_TYPE[im["dim"]],
                           "w": im["w"], "h": im["h"], "depth": im["depth"], "layers": im["layers"],
                           "fmt": im["fmt"], "uav": im["uav"], "data": im["data"].hex()})
        return {"idx": self.idx, "wave": WAVE,
                "code": self.code.hex(), "sgpr": sg, "vgpr": vgpr,
                "regions": regions,
                "data": [f"0x{self.base[r]:x}" for r in self.data_regions],
                "images": images, "hash": hashes}


def empty_scenario(idx):
    s = Scenario(idx)
    s.ptrs, s.ptrhi, s.pokes, s.user = [], [], [], {}
    return s


def build_lines(idx, bins, binds, workdir):
    """run_real.build, extended with the R32_UINT/cube image classes it rejects."""
    try:
        return R.build(idx, WAVE, workdir, WAVE, "float")
    except R.Unsupported:
        pass
    nodes, descs = R.parse_bind(os.path.join(binds, f"{idx}.bind"))
    rnd = random.Random(int(idx))
    user_dwords = max([s[1] + 1 for s in nodes.values() if s[0] == "user"] or [0])
    lines = [f"wave {WAVE}", "groups 1 1 1", f"threads {WAVE} 1 1",
             f"code {os.path.join(bins, f'{idx}.bin')}", f"out {workdir}/out",
             f"userdwords {user_dwords}", "lds 4096", "lowguest"]
    info = {"data": [], "images": []}
    for nid in nodes:
        lines.append(f"region {100 + nid} {NODE_BYTES}")
    for nid, spec in nodes.items():
        if spec[0] == "user":
            lines.append(f"user32 {spec[1]} {100 + nid}")
        else:
            parent, off = spec[1], spec[2]
            lines += [f"ptrlo {100 + parent} {off} {100 + nid}",
                      f"ptrhi {100 + parent} {off + 4} {100 + nid} 0"]
    data_id, image_id = 200, 1
    for node, off, cls in descs:
        if node not in nodes:
            continue
        base = 100 + node
        if cls == "buf":
            p = os.path.join(workdir, f"data{data_id}.bin")
            open(p, "wb").write(R.BLOBS["float"])
            lines.append(f"region {data_id} {DATA_BYTES} init {p}")
            lines += [f"ptrlo {base} {off} {data_id}", f"ptrhi {base} {off + 4} {data_id} {16 << 16}",
                      f"poke {base} {off + 8} {DATA_BYTES // 16}", f"poke {base} {off + 12} 0x1004dfac"]
            info["data"].append(data_id)
            data_id += 1
        elif cls.startswith("img"):
            ip = os.path.join(workdir, f"img{image_id}.bin")
            irnd = random.Random(1000 + image_id)
            f = lambda: struct.pack("<f", 0.25 + irnd.random() * 3.75)
            if cls == "img0":
                open(ip, "wb").write(b"".join(f() for _ in range(IMG * 4)))
                lines += [f"image {image_id} {IMG} 1 dim 1d init {ip}", f"timg {base} {off} {image_id}"]
            elif cls == "img2":
                open(ip, "wb").write(b"".join(f() for _ in range(IMG3D ** 3 * 4)))
                lines += [f"image {image_id} {IMG3D} {IMG3D} dim 3d depth {IMG3D} init {ip}",
                          f"timg {base} {off} {image_id}"]
            elif cls == "img3":
                open(ip, "wb").write(b"".join(f() for _ in range(IMG * IMG * 4 * 6)))
                lines += [f"image {image_id} {IMG} {IMG} dim cube layers 6 init {ip}",
                          f"timg {base} {off} {image_id}"]
            elif cls in ("img1a", "img1b"):
                open(ip, "wb").write(b"".join(f() for _ in range(IMG * IMG)))
                lines += [f"image {image_id} {IMG} {IMG} fmt r32u init {ip}", f"timg {base} {off} {image_id}"]
            elif cls == "img2a":
                open(ip, "wb").write(b"".join(f() for _ in range(IMG3D ** 3)))
                lines += [f"image {image_id} {IMG3D} {IMG3D} dim 3d depth {IMG3D} fmt r32u init {ip}",
                          f"timg {base} {off} {image_id}"]
            else:
                layers = LAYERS if cls == "img5" else 1
                open(ip, "wb").write(b"".join(f() for _ in range(IMG * IMG * 4 * layers)))
                lines += [f"image {image_id} {IMG} {IMG}" + (f" layers {layers}" if layers > 1 else "") +
                          f" init {ip}", f"timg {base} {off} {image_id}"]
            info["images"].append(image_id)
            image_id += 1
        elif cls != "samp":
            raise R.Unsupported("image class " + cls)
    return lines, info


def build_one(idx, bins, binds):
    with tempfile.TemporaryDirectory(prefix="s4sc_") as d:
        lines, info = build_lines(idx, bins, binds, d)
        s = empty_scenario(idx)
        img_cls = {}
        for line in open(os.path.join(binds, f"{idx}.bind")):
            w = line.split()
            if len(w) >= 4 and w[0] == "desc" and w[3].startswith("img"):
                img_cls[len(img_cls) + 1] = w[3]
        for line in lines:
            w = line.split()
            cmd = w[0]
            if cmd == "region":
                rid = int(w[1])
                data = bytearray(int(w[2]))
                if "init" in w and len(w) > w.index("init") + 1:
                    blob = Path(w[w.index("init") + 1]).read_bytes()
                    data[:min(len(data), len(blob))] = blob[:len(data)]
                s.regions[rid] = data
            elif cmd == "image":
                iid, wd, ht = int(w[1]), int(w[2]), int(w[3])
                dim = w[w.index("dim") + 1] if "dim" in w else "2d"
                depth = int(w[w.index("depth") + 1]) if "depth" in w else 1
                layers = int(w[w.index("layers") + 1]) if "layers" in w else 1
                if "r32u" in w:
                    fmt, texels = 4, wd * ht * (depth if dim == "3d" else layers)
                else:
                    fmt, texels = 77, wd * ht * 4 * (depth if dim == "3d" else layers)
                blob = Path(w[w.index("init") + 1]).read_bytes() if "init" in w else bytes(texels * (4 if fmt == 4 else 4))
                data = bytes(blob[:texels * (4 if fmt == 4 else 4)]).ljust(texels * (4 if fmt == 4 else 4), b"\0")
                s.images[iid] = {"w": wd, "h": ht, "dim": dim, "depth": depth, "layers": layers,
                                 "fmt": fmt, "data": data, "uav": img_cls.get(iid, "").endswith(("a", "b"))}
            elif cmd == "ptrlo":
                s.ptrs.append((int(w[1]), int(w[2], 0), int(w[3]), int(w[4], 0) if len(w) > 4 else 0))
            elif cmd == "ptrhi":
                s.ptrhi.append((int(w[1]), int(w[2], 0), int(w[3]), int(w[4], 0) if len(w) > 4 else 0))
            elif cmd == "poke":
                s.pokes.append((int(w[1]), int(w[2], 0), int(w[3], 0)))
            elif cmd == "user32":
                s.user[int(w[1])] = ("r", int(w[2]))
            elif cmd == "userval":
                s.user[int(w[1])] = ("v", int(w[2], 0))
            elif cmd == "timg":
                s.timg.append((int(w[1]), int(w[2], 0), int(w[3])))
        s.data_regions = info["data"]
        s.code = Path(os.path.join(bins, f"{idx}.bin")).read_bytes()
        return s.finish()


def copy_scenario():
    """Minimal load-dword/store-dword shader, same scenario format and hashes."""
    s = empty_scenario("copy")
    s.regions[1] = bytearray(64)
    s.regions[2] = bytearray(64)
    s.ptrs.append((1, 0, 2, 0))
    s.pokes.append((1, 4, 16))       # V# stride
    s.pokes.append((1, 8, 4))        # V# num_records
    s.pokes.append((1, 12, 0x1004DFAC))
    s.pokes.append((2, 0, 0x12345678))
    s.user[4] = ("r", 1)             # V# address, rewritten to the compact base
    s.data_regions = [2]
    # BUFFER_LOAD_DWORD v1, v0, s[4:7], off 0; BUFFER_STORE_DWORD v1, v0, s[4:7]; S_ENDPGM
    load = struct.pack("<II", 0xE0000000, (1 << 8) | (1 << 16) | (128 << 24))
    store = struct.pack("<II", 0xE0140000, (1 << 8) | (1 << 16) | (128 << 24))
    s.code = load + store + struct.pack("<I", 0xBF810000)
    sc = s.finish()
    sc["idx"] = "copy"
    return sc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default="/tmp/opencode/s4-cmp-corpus/manifest_all.csv")
    ap.add_argument("--bins", default="/tmp/opencode/s4-cmp-corpus/bins_all")
    ap.add_argument("--binds", default="/tmp/opencode/s4-cmp-corpus/binds_all")
    ap.add_argument("--out", required=True)
    ap.add_argument("--ids", default="")
    args = ap.parse_args()
    R.ROOT = str(Path(args.bins).parent)
    R.BINS_DIR, R.BINDS_DIR, R.MANIFEST = args.bins, args.binds, args.manifest
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rows = [r for r in csv.DictReader(open(args.manifest)) if r["stage"] == "CS"]
    ids = [i for i in args.ids.split(",") if i] or [r["idx"] for r in rows]
    names = {r["idx"]: r["name"] for r in rows}
    for i, idx in enumerate(ids, 1):
        try:
            sc = build_one(idx, args.bins, args.binds)
        except Exception as e:
            sc = {"idx": idx, "name": names.get(idx, ""), "error": f"{type(e).__name__}: {str(e)[:200]}"}
        sc["name"] = names.get(idx, "")
        (out / f"{idx}.json").write_text(json.dumps(sc, separators=(",", ":")))
        print(f"built {i}/{len(ids)} {idx}", flush=True)
    (out / "copy.json").write_text(json.dumps(copy_scenario(), separators=(",", ":")))


if __name__ == "__main__":
    main()
