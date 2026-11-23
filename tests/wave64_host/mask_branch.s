; synthetic RDNA2 wave64 mesh body: v_cmp mask used for a whole-wave branch
s_or_saveexec_b64 s[0:1], -1
v_mbcnt_lo_u32_b32 v0, -1, 0
v_mbcnt_hi_u32_b32 v0, -1, v0
v_cmp_gt_u32_e64 vcc_lo, 40, v0
s_cbranch_vccz label
v_mov_b32_e32 v1, 200
s_branch done
label:
v_mov_b32_e32 v1, 100
done:
s_mov_b64 exec, s[0:1]
v_mov_b32_e32 v0, v1
exp mrt0, v0, v0, v0, v0 done
s_endpgm
