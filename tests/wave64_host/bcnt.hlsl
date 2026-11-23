// (c) s_bcnt1_i32_b64 of the full EXEC mask on two 32-lane subgroups.
// Synthetic RDNA2 body (bcnt.s): v_cmp_ge_u32 exec, 48, lane_id;
// s_bcnt1_i32_b64 s2, exec. Lanes 48..63 are active (16 lanes); a half-local
// ballot would count 0 in half 0 and 16 in half 1.
RWStructuredBuffer<uint> result : register(u0);
groupshared uint xcl_xchg[66];

[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    uint half = (gi >> 5) & 1u;
    uint lane = WaveGetLaneIndex() + 32u * half;
    uint base = (gi / 64u) * 66u;

    uint4 b = WaveActiveBallot(lane >= 48u);
    if (WaveGetLaneIndex() == 0u) xcl_xchg[base + 64u + half] = b.x;
    GroupMemoryBarrierWithGroupSync();
    uint2 exec = uint2(xcl_xchg[base + 64u], xcl_xchg[base + 65u]);
    GroupMemoryBarrierWithGroupSync();

    result[gi] = countbits(exec.x) + countbits(exec.y);   // 16 in all 64 lanes
}
