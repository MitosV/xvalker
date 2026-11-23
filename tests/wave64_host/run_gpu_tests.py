#!/usr/bin/env python3
"""Run the three HLSL-level wave64-on-32 tests on the host GPU.

Each test assembles its synthetic RDNA2 body with llvm-mc -mcpu=gfx1010, compiles
the HLSL form emitted by the XCL recompiler to SPIR-V with DXC, runs it under
Vulkan with a 64-thread compute dispatch and compares all 64 result lanes.
"""
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DXC = pathlib.Path(os.path.expanduser("~/.cache/xvalker/dxc-linux/unpacked/bin/dxc"))
TMP = pathlib.Path("/tmp/opencode/s2/wave64-gpu")
HARNESS = TMP / "compute_harness"

TESTS = {
    "idempotent": lambda i: 0 if i < 32 else 32,
    "uniform": lambda i: 0xABCDEF01,
    "exchange": lambda i: i + 32 if i < 32 else i - 32,
    "mask_branch": lambda i: 200,
    "readfirstlane": lambda i: 130,
    "bcnt": lambda i: 16,
}


def run(cmd, **kw):
    return subprocess.run([str(x) for x in cmd], check=True, capture_output=True, text=True, **kw)


def main():
    if not shutil.which("llvm-mc") or not shutil.which("llvm-objcopy") or not DXC.exists():
        print("SKIP: llvm-mc, llvm-objcopy or DXC is unavailable")
        return 0
    TMP.mkdir(parents=True, exist_ok=True)
    run(["gcc", "-O2", "-o", HARNESS, HERE / "compute_harness.c", "-lvulkan"])
    failures = 0
    for name, expected in TESTS.items():
        body = TMP / f"{name}.o"
        run(["llvm-mc", "-arch=amdgcn", "-mcpu=gfx1010", "-filetype=obj", "-o", body, HERE / f"{name}.s"])
        obj = TMP / f"{name}.bin"
        run(["llvm-objcopy", "-O", "binary", "--only-section=.text", body, obj])
        spv = TMP / f"{name}.spv"
        run([DXC, "-T", "cs_6_6", "-E", "main", "-spirv", "-fspv-target-env=vulkan1.3", "-Fo", spv, HERE / f"{name}.hlsl"])
        env = dict(os.environ, XV_PREFIX=os.path.expanduser("~/.cache/xvalker/prefix-s2"))
        env.pop("VK_ICD_FILENAMES", None)
        proc = subprocess.run(
            ["flock", os.path.expanduser("~/.cache/xvalker/gpu.lock"), "timeout", "60", str(HARNESS), str(spv)],
            capture_output=True, text=True, env=env,
        )
        if proc.returncode == 3:
            print(f"{name}: SKIPPED ({proc.stderr.strip()})")
            continue
        if proc.returncode != 0:
            print(f"{name}: FAIL (harness exit {proc.returncode}: {proc.stderr.strip()})")
            failures += 1
            continue
        values = [int(x) for x in proc.stdout.split()]
        if len(values) != 64:
            print(f"{name}: FAIL (got {len(values)} lanes)")
            failures += 1
            continue
        wrong = [(i, v, expected(i)) for i, v in enumerate(values) if v != expected(i)]
        if wrong:
            print(f"{name}: FAIL (first mismatch lane {wrong[0][0]}: {wrong[0][1]} != {wrong[0][2]})")
            failures += 1
        else:
            print(f"{name}: PASS (64 lanes, subgroup 32; {proc.stderr.strip()})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
