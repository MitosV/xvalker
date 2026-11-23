; synthetic RDNA2 wave64 idempotent reduction (llvm-mc -mcpu=gfx1010)
s_or_saveexec_b64 s[0:1], -1
v_mbcnt_lo_u32_b32 v6, -1, 0
v_mbcnt_hi_u32_b32 v6, -1, v6
v_min_u32_dpp v6, v6, v6 dpp8:[4,4,4,4,0,0,0,0]
v_min_u32_dpp v6, v6, v6 row_mirror row_mask:0xf bank_mask:0xf bound_ctrl:1
v_permlanex16_b32 v7, v6, 0, 0
v_min_u32_e32 v6, v7, v6
v_readlane_b32 s14, v6, 32
v_min_u32_e32 v6, s14, v6
v_readlane_b32 s53, v6, 0
s_mov_b64 exec, s[0:1]
v_mov_b32_e32 v0, s53
exp mrt0, v0, v0, v0, v0 done
s_endpgm
