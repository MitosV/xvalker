#!/usr/bin/env python3
"""Writes the table of the Xbox One GPU's instructions for the console shader translator.

LLVM does not disassemble for that GPU's instruction set, but it assembles for it: every
instruction the Series table names is written out as LLVM prints it for the Series GPU,
assembled for the Xbox One's, and what comes out says which opcode it has there. Needs
`llvm-mc` (LLVM 14 or later, with the AMDGPU target).

    python tools/gcn_ops.py --llvm-mc "wsl llvm-mc-18" > src/runtime/modules/console_shader_gcn_ops.inc
"""
import argparse
import re
import shlex
import subprocess
import sys

import rdna_ops

CPU = "bonaire"
MARK = "s_nop 1973"
# names the older assembler has for the same instruction
OLDER = {
    "v_add_co_u32": "v_add_i32", "v_sub_co_u32": "v_sub_i32", "v_subrev_co_u32": "v_subrev_i32",
    "v_add_co_ci_u32": "v_addc_u32", "v_sub_co_ci_u32": "v_subb_u32",
    "v_subrev_co_ci_u32": "v_subbrev_u32",
}


# what only the older instruction set has, or has under a name the newer one dropped
EXTRA = """
v_cndmask_b32 v1, v2, v3, vcc
v_min_legacy_f32 v1, v2, v3
v_max_legacy_f32 v1, v2, v3
v_lshr_b32 v1, v2, v3
v_ashr_i32 v1, v2, v3
v_lshl_b32 v1, v2, v3
v_add_i32 v1, vcc, v2, v3
v_sub_i32 v1, vcc, v2, v3
v_subrev_i32 v1, vcc, v2, v3
v_addc_u32 v1, vcc, v2, v3, vcc
v_subb_u32 v1, vcc, v2, v3, vcc
v_subbrev_u32 v1, vcc, v2, v3, vcc
v_cvt_pkaccum_u8_f32 v1, v2, v3
s_load_dword s1, s[2:3], 0x0
s_load_dwordx2 s[4:5], s[2:3], 0x0
s_load_dwordx4 s[4:7], s[2:3], 0x0
s_load_dwordx8 s[4:11], s[2:3], 0x0
s_load_dwordx16 s[4:19], s[2:3], 0x0
s_buffer_load_dword s1, s[4:7], 0x0
s_buffer_load_dwordx2 s[4:5], s[4:7], 0x0
s_buffer_load_dwordx4 s[4:7], s[4:7], 0x0
s_buffer_load_dwordx8 s[4:11], s[4:7], 0x0
s_buffer_load_dwordx16 s[4:19], s[4:7], 0x0
tbuffer_load_format_x v1, v2, s[4:7], dfmt:4, nfmt:7, 0 idxen
tbuffer_load_format_xy v[1:2], v2, s[4:7], dfmt:4, nfmt:7, 0 idxen
tbuffer_load_format_xyz v[1:3], v2, s[4:7], dfmt:4, nfmt:7, 0 idxen
tbuffer_load_format_xyzw v[1:4], v2, s[4:7], dfmt:4, nfmt:7, 0 idxen
tbuffer_store_format_x v1, v2, s[4:7], dfmt:4, nfmt:7, 0 idxen
tbuffer_store_format_xy v[1:2], v2, s[4:7], dfmt:4, nfmt:7, 0 idxen
tbuffer_store_format_xyz v[1:3], v2, s[4:7], dfmt:4, nfmt:7, 0 idxen
tbuffer_store_format_xyzw v[1:4], v2, s[4:7], dfmt:4, nfmt:7, 0 idxen
v_log_clamp_f32 v1, v2
v_rcp_clamp_f32 v1, v2
v_rcp_legacy_f32 v1, v2
v_rsq_clamp_f32 v1, v2
v_rsq_legacy_f32 v1, v2
v_rcp_clamp_f64 v[1:2], v[2:3]
v_rsq_clamp_f64 v[1:2], v[2:3]
v_mullit_f32 v1, v2, v3, v4
v_mul_lo_i32 v1, v2, v3
v_mul_hi_i32 v1, v2, v3
v_mul_lo_u32 v1, v2, v3
v_mul_hi_u32 v1, v2, v3
v_mad_legacy_f32 v1, v2, v3, v4
s_cbranch_g_fork s[2:3], s[4:5]
""".strip().splitlines()


def texts(llvm_mc, encodings):
    """What LLVM prints for each encoding on the Series GPU, or None."""
    stream = [word for words in encodings for word in (*words, rdna_ops.MARK, rdna_ops.MARK, rdna_ops.MARK)]
    text = "".join(" ".join(f"0x{byte:02x}" for byte in word.to_bytes(4, "little")) + chr(10)
                   for word in stream)
    run = subprocess.run(
        shlex.split(llvm_mc) + ["--disassemble", "-triple=amdgcn", f"-mcpu={rdna_ops.CPU}", "--show-encoding"],
        input=text, capture_output=True, text=True)
    mark = ",".join(f"0x{b:02x}" for b in rdna_ops.MARK.to_bytes(4, "little"))
    groups, group, marks = [], [], 0
    lines = [re.match(r"\s*(.*?)\s*(?://|;).*encoding: \[(.*)\]", line) for line in run.stdout.splitlines()]
    for match in [found for found in lines if found] + [None]:
        if match and match.group(2) == mark:
            marks += 1
            continue
        if marks:
            groups.append(group)
            groups += [[] for _ in range((marks + 1) // 3 - 1)]
            group, marks = [], 0
        if match:
            group.append((match.group(1), match.group(2)))
    result = []
    for words, group in zip(encodings, groups):
        whole = ",".join(f"0x{b:02x}" for word in words for b in word.to_bytes(4, "little"))
        result.append(group[0][0] if len(group) == 1 and group[0][1] == whole else None)
    return result + [None] * (len(encodings) - len(result))


def older(text):
    """The statement as the older assembler takes it."""
    name, _, operands = text.partition(" ")
    name = re.sub(r"_(e32|e64)$", "", name)
    operands = re.sub(r"\s+dim:\S+", "", operands)
    operands = re.sub(r"\s+dlc\b", "", operands)
    operands = operands.replace("format:[BUF_FMT_32_FLOAT]", "dfmt:4, nfmt:7")
    operands = re.sub(r"format:\[[A-Z0-9_]+\]", "dfmt:4, nfmt:7", operands)
    return name, f"{OLDER.get(name, name)} {operands}".strip()


def classify(words):
    """The format and opcode of an Xbox One encoding."""
    w = words[0]
    top = w >> 26
    if top == 0x34:
        return "vop3", (w >> 17) & 0x1FF
    if top == 0x32:
        return "vintrp", (w >> 16) & 3
    if top == 0x36:
        return "ds", (w >> 18) & 0xFF
    if top == 0x38:
        return "mubuf", (w >> 18) & 0x7F
    if top == 0x3A:
        return "mtbuf", (w >> 16) & 7
    if top == 0x3C:
        return "mimg", (w >> 18) & 0x7F
    if top == 0x3E:
        return "exp", 0
    if w >> 27 == 0x18:
        return "smem", (w >> 22) & 0x1F
    if w >> 23 == 0x17D:
        return "sop1", (w >> 8) & 0xFF
    if w >> 23 == 0x17E:
        return "sopc", (w >> 16) & 0x7F
    if w >> 23 == 0x17F:
        return "sopp", (w >> 16) & 0x7F
    if w >> 28 == 0xB:
        return "sopk", (w >> 23) & 0x1F
    if w >> 30 == 2:
        return "sop2", (w >> 23) & 0x7F
    if w >> 25 == 0x3F:
        return "vop1", (w >> 9) & 0xFF
    if w >> 25 == 0x3E:
        return "vopc", (w >> 17) & 0xFF
    if w >> 31 == 0:
        return "vop2", (w >> 25) & 0x3F
    return None, 0


def assemble(llvm_mc, statements):
    """The words each statement assembles to on the Xbox One GPU, or None."""
    source = "".join(f"{statement}\n{MARK}\n" for statement in statements)
    run = subprocess.run(
        shlex.split(llvm_mc) + ["-triple=amdgcn", f"-mcpu={CPU}", "--show-encoding"],
        input=source, capture_output=True, text=True)
    result, current = [], None
    for line in run.stdout.splitlines():
        match = re.search(r"encoding: \[(.*)\]", line)
        if not match:
            continue
        data = bytes(int(part, 16) for part in match.group(1).split(","))
        words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
        if line.strip().startswith("s_nop") and words == [0xBF8007B5]:
            result.append(current)
            current = None
        else:
            current = words
    return result + [None] * (len(statements) - len(result))


def build(llvm_mc):
    found = {}
    for fmt in rdna_ops.ORDER:
        tries = [words for op in range(rdna_ops.RANGES[fmt]) for words in rdna_ops.candidates(fmt, op)]
        printed = [text for text in texts(llvm_mc, tries) if text]
        pairs = [older(text) for text in dict.fromkeys(printed)]
        for (name, _), words in zip(pairs, assemble(llvm_mc, [statement for _, statement in pairs])):
            if not words:
                continue
            kind, op = classify(words)
            if kind:
                found.setdefault((kind, op), name)
    for statement, words in zip(EXTRA, assemble(llvm_mc, EXTRA)):
        if words:
            kind, op = classify(words)
            if kind:
                found[(kind, op)] = statement.split()[0]
    return sorted(found.items(), key=lambda item: (rdna_ops.ORDER.index(item[0][0]), item[0][1]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--llvm-mc", default="llvm-mc", help="the assembler's command line")
    arguments = parser.parse_args()
    for (fmt, op), name in build(arguments.llvm_mc):
        sys.stdout.write(f'    {{F::{fmt}, 0x{op:x}, "{name}"}},\n')
    return 0


if __name__ == "__main__":
    sys.exit(main())
