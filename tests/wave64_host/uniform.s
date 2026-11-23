; synthetic RDNA2 wave64 readlane of a wave-uniform value
s_or_saveexec_b64 s[0:1], -1
v_mov_b32_e32 v0, s0
v_readlane_b32 s1, v0, 60
s_mov_b64 exec, s[0:1]
v_mov_b32_e32 v0, s1
exp mrt0, v0, v0, v0, v0 done
s_endpgm
