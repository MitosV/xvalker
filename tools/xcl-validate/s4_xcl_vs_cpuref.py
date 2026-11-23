#!/usr/bin/env python3
"""Compare vendored XCL HLSL on the GPU with americas-recomp cpu_ref.py.

One scenario JSON (built by build_scenarios.py) is loaded by both sides. Subcommands:
  build      - delegate to build_scenarios.py
  run-gpu    - run the XCL kernel test under the GPU lock
  compare    - input self-check, run cpu_ref, compare written regions
  bisect     - truncate each differing shader after N instructions and find the
               first instruction whose register result differs
"""
import argparse, collections, csv, hashlib, json, os, struct, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = Path(os.environ.get("XBOX_TOOLS", "/tmp/opencode/americas-s4/tools/xbox"))
os.environ["XBOX_WAVE"] = "32"
os.environ["XBOX_RDNA2"] = "recomp"
sys.path.insert(0, str(TOOLS))
import cpu_ref as CR  # noqa: E402

WAVE = 32
STEPS = 100000
GPU_LOCK = "/home/mitosv/.cache/xvalker/gpu.lock"
WINE = "/home/mitosv/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine"
XCL_EXE = "/tmp/opencode/xcl-s4/target/x86_64-pc-windows-gnu/release/deps/xcl-309ae8bc4b65ff3c.exe"
DIMS = {8: "1d", 9: "2d", 10: "3d", 11: "cube", 13: "2da"}


def load_scenario(path):
    return json.loads(Path(path).read_text())


def hex_bytes(s):
    return bytes.fromhex(s)


def run_cpu(sc, registers=False):
    mem = CR.Memory()
    for r in sc["regions"]:
        mem.add(r["base"], bytearray(hex_bytes(r["data"])))
    sgpr = {i: v for i, v in enumerate(sc["sgpr"])}
    wave = CR.Wave(hex_bytes(sc["code"]), mem, sgpr)
    for lane, row in enumerate(sc["vgpr"]):
        for reg, v in enumerate(row):
            if v:
                wave.v[reg][lane] = v
    wave.s[127] = 0
    wave.images = {}
    for im in sc["images"]:
        words = struct.unpack(f"<{len(im['data']) // 8}I", hex_bytes(im["data"]))
        wave.images[im["id"]] = {"w": im["w"], "h": im["h"], "depth": im["depth"],
                                 "layers": im["layers"], "dim": DIMS.get(im["dim"], "2d"),
                                 "data": list(words), "written": {}}
    err = None
    try:
        wave.run(STEPS)
    except Exception as e:
        err = f"{type(e).__name__}: {str(e)[:200]}"
    data, images = {}, {}
    for addr in sc["data"]:
        base = int(addr, 16)
        r = next((r for r in sc["regions"] if r["base"] == base), None)
        if r:
            d = mem.find(base, len(r["data"]) // 2)[0]
            data[addr] = bytes(d).hex() if d is not None else None
    for im in sc["images"]:
        if im["uav"]:
            words = wave.images.get(im["id"], {}).get("data", [])
            images[str(im["srv_index"])] = struct.pack(f"<{len(words)}I", *words).hex()
    out = {"ok": err is None, "err": err, "data": data, "images": images,
           "steps": getattr(wave, "steps", None), "approx": getattr(wave, "approx", None)}
    if registers:
        out["regs"] = {"sgpr": struct.pack("<128I", *wave.s[:128]).hex(),
                       "vgpr": [struct.pack("<256I", *[wave.v[r][l] for r in range(256)]).hex()
                                for l in range(WAVE)]}
    return out


def first_diff(cpu_hex, gpu_hex):
    if cpu_hex is None or gpu_hex is None:
        return {"reason": "missing region"}
    a, b = hex_bytes(cpu_hex), hex_bytes(gpu_hex)
    for i in range(0, min(len(a), len(b)), 4):
        c, g = struct.unpack_from("<I", a, i)[0], struct.unpack_from("<I", b, i)[0]
        if c != g:
            return {"off": i, "cpu": hex(c), "gpu": hex(g)}
    if len(a) != len(b):
        return {"off": min(len(a), len(b)), "cpu": f"len{len(a)}", "gpu": f"len{len(b)}"}
    return None


def input_mismatch(sc, rec):
    want, got = sc.get("hash", {}), rec.get("input_hash", {})
    for k, v in want.items():
        if got.get(k) != v:
            return f"input mismatch: {k}: expected {v}, got {got.get(k)}"
    return None


def compare_one(sc, rec):
    bad = input_mismatch(sc, rec)
    if bad:
        return {"outcome": "NOT_RUN", "reason": bad}
    if rec.get("outcome") != "RAN":
        return {"outcome": "NOT_RUN", "reason": "xcl:" + str(rec.get("reason", "?"))[:200]}
    cpu = run_cpu(sc)
    if not cpu["ok"]:
        return {"outcome": "NOT_RUN", "reason": "cpu:" + str(cpu["err"])[:200]}
    diffs, first = 0, None
    for addr, gpu in rec.get("data", {}).items():
        d = first_diff(cpu["data"].get(addr), gpu)
        if d:
            diffs += 1
            first = first or {"kind": "data", "addr": addr, **d}
    for k, gpu in rec.get("images", {}).items():
        d = first_diff(cpu["images"].get(k, ""), gpu)
        if d:
            diffs += 1
            first = first or {"kind": "image", "srv": k, **d}
    out = {"outcome": "EQUAL" if diffs == 0 else "DIFFERENT", "diff_count": diffs,
           "cpu_steps": cpu["steps"], "input_check": "pass"}
    if first:
        out["first_diff"] = first
    return out


def win(p):
    return "Z:" + str(p).replace("/", "\\")


def run_gpu(scenarios, out, ids, regs=False, batch=10, timeout=600, exe=XCL_EXE):
    env = os.environ.copy()
    env.update({"XCL_CMP_SCENARIOS": win(scenarios), "XCL_CMP_OUT": win(out),
                "XCL_CMP_IDS": ",".join(ids),
                "WINEPREFIX": "/home/mitosv/.cache/xvalker/prefix-s4/pfx",
                "STEAM_COMPAT_DATA_PATH": "/home/mitosv/.cache/xvalker/prefix-s4",
                "STEAM_COMPAT_CLIENT_INSTALL_PATH": "/home/mitosv/.steam/steam",
                "WINEDLLOVERRIDES": "d3d12,d3d12core=n", "WINEDEBUG": "-all",
                "WINEESYNC": "1", "WINEFSYNC": "1"})
    if regs:
        env["XCL_CMP_REGS"] = "1"
    cmd = ["flock", GPU_LOCK, "timeout", str(timeout), WINE, os.path.basename(exe),
           "xcl_cmp_manifest", "--ignored", "--nocapture", "--test-threads=1"]
    log, all_recs = [], []
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    for i in range(0, len(ids), batch):
        part = Path(f"{out}.batch{i // batch:03d}.jsonl")
        env["XCL_CMP_OUT"] = win(part)
        env["XCL_CMP_IDS"] = ",".join(ids[i:i + batch])
        t0 = time.time()
        p = subprocess.run(cmd, cwd=os.path.dirname(exe), env=env, capture_output=True, text=True)
        log.append(f"batch {i // batch}: rc={p.returncode} dt={time.time() - t0:.1f}s\n{p.stdout[-2000:]}\n{p.stderr[-2000:]}")
        recs = [json.loads(x) for x in part.read_text().splitlines() if x.strip()] if part.exists() else []
        all_recs.extend(recs)
        print(f"batch {i // batch + 1}/{(len(ids) + batch - 1) // batch} rc={p.returncode} dt={time.time() - t0:.1f}s records={len(recs)}", flush=True)
    Path(str(out) + ".log").write_text("\n".join(log))
    Path(out).write_text("".join(json.dumps(r) + "\n" for r in all_recs))
    return all_recs


def cmd_build(a):
    subprocess.check_call([sys.executable, str(HERE / "build_scenarios.py"),
                           "--manifest", a.manifest, "--bins", a.bins, "--binds", a.binds,
                           "--out", a.scenarios, "--ids", a.ids])


def cmd_run_gpu(a):
    ids = a.ids.split(",") if a.ids else [p.stem for p in sorted(Path(a.scenarios).glob("*.json")) if p.stem != "copy"]
    recs = run_gpu(a.scenarios, a.out, ids, regs=a.regs, batch=a.batch, timeout=a.timeout, exe=a.exe)
    print("gpu records", len(recs), "ran", sum(1 for r in recs if r.get("outcome") == "RAN"))


def cmd_compare(a):
    recs = {}
    for line in Path(a.xcl_jsonl).read_text().splitlines():
        if line.strip():
            r = json.loads(line)
            recs[r["idx"]] = r
    results, ids = [], a.ids.split(",") if a.ids else [p.stem for p in sorted(Path(a.scenarios).glob("*.json")) if p.stem != "copy"]
    for idx in ids:
        sc = load_scenario(Path(a.scenarios) / f"{idx}.json")
        if "error" in sc:
            rec = {"outcome": "NOT_RUN", "reason": "scenario:" + sc["error"][:200]}
        else:
            rec = compare_one(sc, recs.get(idx, {"outcome": "NOT_RUN", "reason": "missing GPU record"}))
        results.append({"idx": idx, "name": sc.get("name", ""), **rec})
    Path(a.out).write_text(json.dumps(results, indent=2))
    by = collections.Counter(r["outcome"] for r in results)
    print("counts", dict(by))
    reasons = collections.Counter(r.get("reason", "?").split(":")[0] for r in results if r["outcome"] == "NOT_RUN")
    print("not-run reasons", reasons.most_common(12))
    for r in results:
        if r["outcome"] == "DIFFERENT":
            print("DIFF", r["idx"], r.get("diff_count"), r.get("first_diff"))


def cmd_copy_check(a):
    sc = load_scenario(Path(a.scenarios) / "copy.json")
    recs = run_gpu(a.scenarios, a.out, ["copy"], timeout=a.timeout, exe=a.exe)
    rec = recs[0]
    mismatch = input_mismatch(sc, rec)
    cpu = run_cpu(sc)
    gpu_val = hex_bytes(rec.get("data", {}).get(sc["data"][0], "")) if rec.get("outcome") == "RAN" else b""
    cpu_val = hex_bytes(cpu["data"].get(sc["data"][0], "")) if cpu["ok"] else b""
    ok = mismatch is None and rec.get("outcome") == "RAN" and cpu["ok"] and gpu_val == cpu_val
    print(json.dumps({"input_check": mismatch or "pass", "gpu": rec.get("outcome"),
                      "cpu": cpu["err"] or "RAN", "gpu_data": gpu_val.hex(),
                      "cpu_data": cpu_val.hex(), "pass": ok}, indent=2))
    if not ok:
        raise SystemExit(1)


def instruction_offsets(code):
    offs, off = [], 0
    while off < len(code):
        i = CR.rdna2.decode(code, off)
        offs.append(off)
        off += i.size
    return offs


def reg_diff(cpu, gpu, sc):
    """Compare only registers that both interpreters actually wrote.

    XCL emits final SGPR/VGPR stores only for registers its translation uses, so an
    untouched register reads back as 0 on the GPU while the CPU keeps its input value;
    that is not a semantic difference.
    """
    init_s = sc["sgpr"]
    init_v = sc["vgpr"]
    a, b = hex_bytes(cpu["regs"]["sgpr"]), hex_bytes(gpu["regs"]["sgpr"])
    for i in range(0, min(len(a), len(b)), 4):
        c, g = struct.unpack_from("<I", a, i)[0], struct.unpack_from("<I", b, i)[0]
        if c != g and c != init_s[i // 4] and g != init_s[i // 4]:
            return f"s{i // 4}: cpu=0x{c:x} gpu=0x{g:x}"
    for lane, (cl, gl) in enumerate(zip(cpu["regs"]["vgpr"], gpu["regs"]["vgpr"])):
        a, b = hex_bytes(cl), hex_bytes(gl)
        for i in range(0, min(len(a), len(b)), 4):
            c, g = struct.unpack_from("<I", a, i)[0], struct.unpack_from("<I", b, i)[0]
            if c != g and c != init_v[lane][i // 4] and g != init_v[lane][i // 4]:
                return f"v{i // 4}[{lane}]: cpu=0x{c:x} gpu=0x{g:x}"
    return None


def output_diff(sc, cpu, gpu):
    for addr, g in gpu.get("data", {}).items():
        d = first_diff(cpu["data"].get(addr), g)
        if d:
            return {"kind": "data", "addr": addr, **d}
    for k, g in gpu.get("images", {}).items():
        d = first_diff(cpu["images"].get(k, ""), g)
        if d:
            return {"kind": "image", "srv": k, **d}
    return None


def cmd_bisect(a):
    Path(a.out).mkdir(parents=True, exist_ok=True)
    ids = a.ids.split(",")
    variants, meta = [], {}
    for idx in ids:
        sc = load_scenario(Path(a.scenarios) / f"{idx}.json")
        code = hex_bytes(sc["code"])
        offs = instruction_offsets(code)
        meta[idx] = {"offs": offs, "name": sc.get("name", "")}
        for n in range(1, min(len(offs), a.max_inst) + 1):
            vid = f"{idx}_n{n}"
            v = dict(sc)
            v["code"] = (code[:offs[n - 1]] + struct.pack("<I", 0xBF810000)).hex()
            h = dict(sc["hash"])
            h["code"] = fnv(hex_bytes(v["code"]))
            v["hash"] = h
            (Path(a.out) / f"{vid}.json").write_text(json.dumps(v, separators=(",", ":")))
            variants.append(vid)
    recs = run_gpu(a.out, str(Path(a.out) / "gpu.jsonl"), variants, regs=True,
                   batch=a.batch, timeout=a.timeout, exe=a.exe)
    by = {r["idx"]: r for r in recs}
    out = []
    for idx in ids:
        offs = meta[idx]["offs"]
        lo, hi = 1, min(len(offs), a.max_inst)
        best = None
        while lo <= hi:
            mid = (lo + hi) // 2
            vid = f"{idx}_n{mid}"
            sc = load_scenario(Path(a.out) / f"{vid}.json")
            r = by.get(vid, {})
            if r.get("outcome") != "RAN" or input_mismatch(sc, r):
                lo = mid + 1
                continue
            cpu = run_cpu(sc, registers=True)
            d = None if not cpu["ok"] else (output_diff(sc, cpu, r) or reg_diff(cpu, r, sc))
            if d:
                best = (mid, d)
                hi = mid - 1
            else:
                lo = mid + 1
        if best:
            n, d = best
            ins = CR.rdna2.decode(hex_bytes(load_scenario(Path(a.scenarios) / f"{idx}.json")["code"]), offs[n - 1])
            out.append({"idx": idx, "name": meta[idx]["name"], "instruction_number": n,
                        "byte_offset": hex(offs[n - 1]), "instruction": ins.name,
                        "first_difference": d})
        else:
            out.append({"idx": idx, "name": meta[idx]["name"], "instruction_number": None,
                        "reason": "no difference in any prefix"})
        print(out[-1], flush=True)
    Path(a.outJson).write_text(json.dumps(out, indent=2))


def fnv(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & ((1 << 64) - 1)
    return f"{h:016x}"


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--manifest", default="/tmp/opencode/s4-cmp-corpus/manifest_all.csv")
    b.add_argument("--bins", default="/tmp/opencode/s4-cmp-corpus/bins_all")
    b.add_argument("--binds", default="/tmp/opencode/s4-cmp-corpus/binds_all")
    b.add_argument("--scenarios", default="/tmp/opencode/s4-r3/scenarios")
    b.add_argument("--ids", default="")
    b.set_defaults(func=cmd_build)
    g = sub.add_parser("run-gpu")
    g.add_argument("--scenarios", default="/tmp/opencode/s4-r3/scenarios")
    g.add_argument("--out", default="/tmp/opencode/s4-r3/gpu.jsonl")
    g.add_argument("--ids", default="")
    g.add_argument("--regs", action="store_true")
    g.add_argument("--batch", type=int, default=10)
    g.add_argument("--timeout", type=int, default=600)
    g.add_argument("--exe", default=XCL_EXE)
    g.set_defaults(func=cmd_run_gpu)
    c = sub.add_parser("compare")
    c.add_argument("--scenarios", default="/tmp/opencode/s4-r3/scenarios")
    c.add_argument("--xcl-jsonl", default="/tmp/opencode/s4-r3/gpu.jsonl")
    c.add_argument("--out", default="/tmp/opencode/s4-r3/cmp.json")
    c.add_argument("--ids", default="")
    c.set_defaults(func=cmd_compare)
    k = sub.add_parser("copy-check")
    k.add_argument("--scenarios", default="/tmp/opencode/s4-r3/scenarios")
    k.add_argument("--out", default="/tmp/opencode/s4-r3/copy.jsonl")
    k.add_argument("--timeout", type=int, default=600)
    k.add_argument("--exe", default=XCL_EXE)
    k.set_defaults(func=cmd_copy_check)
    z = sub.add_parser("bisect")
    z.add_argument("--scenarios", default="/tmp/opencode/s4-r3/scenarios")
    z.add_argument("--ids", required=True)
    z.add_argument("--out", default="/tmp/opencode/s4-r3/bisect")
    z.add_argument("--outJson", default="/tmp/opencode/s4-r3/bisect.json")
    z.add_argument("--max-inst", type=int, default=256)
    z.add_argument("--batch", type=int, default=12)
    z.add_argument("--timeout", type=int, default=600)
    z.add_argument("--exe", default=XCL_EXE)
    z.set_defaults(func=cmd_bisect)
    a = ap.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
