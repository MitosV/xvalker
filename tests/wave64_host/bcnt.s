; synthetic RDNA2 wave64 mesh body: s_bcnt1 of the full EXEC mask
s_or_saveexec_b64 s[0:1], -1
v_mbcnt_lo_u32_b32 v0, -1, 0
v_mbcnt_hi_u32_b32 v0, -1, v0
v_cmp_ge_u32_e64 exec_lo, 48, v0
s_bcnt1_i32_b64 s2, exec
s_mov_b64 exec, s[0:1]
v_mov_b32_e32 v0, s2
exp mrt0, v0, v0, v0, v0 done
s_endpgm
