# xcl-validate: vendored XCL HLSL vs `cpu_ref.py`

Validates the HLSL emitted by xvalker's vendored XCL translator against
`americas-recomp`'s `cpu_ref.py` on real GTA RDNA2 compute shaders.

The central fix in this round (s4-r3) is a **single serialized scenario per shader**.
`build_scenarios.py` calls `run_real.build()` (the same source of truth the CPU
reference uses), rewrites all pointers/V#/T# words to compact guest addresses, and
writes one JSON file containing code, 128 SGPRs, 32x256 VGPRs, every memory region
and every image. Both sides load that file; neither reconstructs inputs from bind
files. Both sides FNV-1a hash every region, SGPR, VGPR, code and image before running,
and a shader is reported `NOT_RUN: input mismatch` if any hash differs.

## Layout

- `build_scenarios.py` — scenario builder (reads `run_real.py`, emits JSON + hashes).
- `s4_xcl_vs_cpuref.py` — driver: `build`, `copy-check`, `run-gpu`, `compare`, `bisect`.
- `cpu_ref.patch` — 32-lane wave + XCL T# decoding, applied to a **copy** of americas.
- `xcl_tests.patch` — appends the env-driven `xcl_cmp_manifest` test to a **copy** of XCL.
  Applies cleanly to upstream `dd713e3` (verified with `patch --dry-run`).

Nothing in `/mnt/games` or the reference repositories is modified.

## Exact commands (as run for s4-r3)

```sh
# 0. Copies under /tmp; XCL at upstream dd713e3 plus the local Wine workaround.
cp -a ~/dev/projects/xcl /tmp/opencode/xcl-s4
cp -a ~/dev/projects/ref/americas-recomp /tmp/opencode/americas-s4   # path used here
cd /tmp/opencode/americas-s4
patch -p1 < /home/mitosv/dev/projects/xvalker-wt/s4/tools/xcl-validate/cpu_ref.patch
cd /tmp/opencode/xcl-s4
patch -p1 < /home/mitosv/dev/projects/xvalker-wt/s4/tools/xcl-validate/xcl_tests.patch
```

```sh
# 1. Build the ignored Rust test binary (Rust target: x86_64-pc-windows-gnu).
cd /tmp/opencode/xcl-s4
cargo test -p xcl --no-run --release --target x86_64-pc-windows-gnu
# exe: target/x86_64-pc-windows-gnu/release/deps/xcl-309ae8bc4b65ff3c.exe
# copy d3d12.dll, d3d12core.dll, dxcompiler.dll, dxil.dll next to it
```

```sh
# 2. Build the 100 serialized scenarios + the trivial copy shader.
cd /home/mitosv/dev/projects/xvalker-wt/s4
XBOX_RDNA2=recomp python3 tools/xcl-validate/build_scenarios.py \
  --manifest /tmp/opencode/s4-cmp-corpus/manifest_all.csv \
  --bins /tmp/opencode/s4-cmp-corpus/bins_all \
  --binds /tmp/opencode/s4-cmp-corpus/binds_all \
  --out /tmp/opencode/s4-r3/scenarios
```

```sh
# 3. Copy canary (BUFFER_LOAD_DWORD + BUFFER_STORE_DWORD); must print pass: true.
python3 tools/xcl-validate/s4_xcl_vs_cpuref.py copy-check \
  --scenarios /tmp/opencode/s4-r3/scenarios --out /tmp/opencode/s4-r3/copy.jsonl
```

```sh
# 4. Re-run all 100 CS on the GPU, one shader per process (under the GPU lock).
python3 tools/xcl-validate/s4_xcl_vs_cpuref.py run-gpu \
  --scenarios /tmp/opencode/s4-r3/scenarios --out /tmp/opencode/s4-r3/gpu_all.jsonl \
  --batch 1 --timeout 600
```

The driver invokes, for each batch:

```sh
WINEPREFIX=~/.cache/xvalker/prefix-s4/pfx \
STEAM_COMPAT_DATA_PATH=~/.cache/xvalker/prefix-s4 \
STEAM_COMPAT_CLIENT_INSTALL_PATH=/home/mitosv/.steam/steam \
WINEDLLOVERRIDES='d3d12,d3d12core=n' WINEDEBUG=-all WINEESYNC=1 WINEFSYNC=1 \
XCL_CMP_SCENARIOS='Z:\tmp\opencode\s4-r3\scenarios' \
XCL_CMP_OUT='Z:\tmp\opencode\s4-r3\gpu_all.jsonl.batch000.jsonl' \
XCL_CMP_IDS=<ids> \
flock ~/.cache/xvalker/gpu.lock timeout 600 \
'/home/mitosv/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine' \
xcl-309ae8bc4b65ff3c.exe xcl_cmp_manifest --ignored --nocapture --test-threads=1
```

```sh
# 5. Compare (re-hashes both sides, runs cpu_ref.py 100000 steps, first-dword diff).
python3 tools/xcl-validate/s4_xcl_vs_cpuref.py compare \
  --scenarios /tmp/opencode/s4-r3/scenarios \
  --xcl-jsonl /tmp/opencode/s4-r3/gpu_all.jsonl \
  --out /tmp/opencode/s4-r3/cmp_all.json
```

```sh
# 6. Bisect any remaining difference: cut code after N instructions (append
#    S_ENDPGM), run GPU + CPU with --regs, and binary-search the first prefix
#    whose output or written register differs.
python3 tools/xcl-validate/s4_xcl_vs_cpuref.py bisect \
  --scenarios /tmp/opencode/s4-r3/scenarios --ids 23465 \
  --out /tmp/opencode/s4-r3/bisect --outJson /tmp/opencode/s4-r3/bisect.json \
  --max-inst 25 --batch 5 --timeout 600
```

## s4-r3 results

- Input self-check: 100/100 shaders hash-equal on code, SGPRs, VGPRs, all regions and images; 0 input mismatches.
- Copy canary: `input_check: pass`, both sides stored `0x12345678` — equal.
- CS counts: equal 32, different 1, not run 67 (XCL GPU timeout 23, XCL decode error 8, cpu_ref unsupported 36).
- Remaining difference `23465`: first differing register at instruction 15, byte `0x54`, `v_fmac_f32` (`v1[0]`).
- Artifacts: `/tmp/opencode/s4-r3/{scenarios,gpu_all.jsonl,cmp_all.json,bisect.json,copy.jsonl}`.

The 23 GPU timeouts are `GPU did not finish within 10 s (device removed: Ok(()))`
from XCL's runner; they are reported as not-run, not as equal. The 8 XCL
`cannot decode` errors and the cpu_ref unsupported cases are harness coverage
limits, not shader result mismatches.

## Exact program code (s4-r4)

The carved `.bin` files append the FSSM constant data blocks after the program's
last `s_endpgm` (44–84 bytes). XCL classifies them as `data_blocks`, not code; their
first word after the guard `s_endpgm` is a constant such as `0x00000008`, which the
translator reports as `cannot decode (Unknown(8))`. To use the program exactly as
XCL parses it:

```sh
cd tools/xcl-recomp
cargo build --release
./target/release/xcl-corpus /mnt/games/GTA/GTA6.Xbox.v1.0.2.1/shaders.rpf \
  --dump-code /tmp/opencode/s4-r4/xcl-codes.json
cd ../xcl-validate
python3 build_scenarios.py --xcl-code /tmp/opencode/s4-r4/xcl-codes.json \
  --out /tmp/opencode/s4-r4/scenarios
```

The builder strips the same 6-instruction (24-byte) entry prologue as the carve and
uses `f.programs[p.offset..p.offset+p.len]`, so no data block reaches the translator.
All 8 shaders that previously failed to decode now run on the GPU; their CPU
reference is unsupported (`v_cos_f32`, `s_getpc_b64`, `v_xnor_b32`, `v_mul_hi_i32`,
a16/d16 image access, non-finite sample coordinate).

## GPU timeout study (s4-r4)

`xcl_timeout.patch` makes the runner fence timeout (and, for the experiment, the
emitted `xcl_budget` loop guard) env-driven in the Windows test build:

```sh
cd /tmp/opencode/xcl-s4 && patch -p1 < <worktree>/tools/xcl-validate/xcl_timeout.patch
cargo test -p xcl --no-run --release --target x86_64-pc-windows-gnu
XCL_CMP_GPU_TIMEOUT_MS=15000 python3 s4_xcl_vs_cpuref.py run-gpu \
  --scenarios /tmp/opencode/s4-r4/scenarios-all \
  --out /tmp/opencode/s4-r4/gpu-all.jsonl --batch 1 --timeout 30
```

The hardcoded error text still says "10 s" even when the env timeout is used.
Results: raising the fence to 30–60 s and shrinking `xcl_budget` to 256 did **not**
make any of the 23 timeouts finish. Each ran in its own wine process (a hang poisons
the device: later shaders in the same process read back an all-zero `mem` buffer, and
even a following copy canary returns zeroes instead of `0x12345678`).

Classification: 2 hit the cpu_ref step limit (`22340`, `24652`) — fake-input
infinite loops. The other 21 are GPU-only hangs. 13 of them contain a guarded
`[loop] for(;;)` / `while (pc ...)` in the emitted HLSL (e.g. `22340` at `+0x110`,
`24652` at `+0x1f0`); 9 more are straight-line HLSL and `22264` is a VS program the
CS dump does not cover — for `22320` a padded prefix bisect isolates the stall to
`buffer_load_format_d16_xyzw` at `+0x70` (prefix 20 runs, prefix 21 never signals
the fence). All 23 stay not-run.
