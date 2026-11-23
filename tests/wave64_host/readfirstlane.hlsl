// (b) v_readfirstlane of lane data on two 32-lane subgroups.
// Synthetic RDNA2 body (readfirstlane.s): v_cmp_gt_u32 exec, 40, lane_id;
// v_readfirstlane_b32 s2, lane_id. The first active lane (41) is in the upper
// half, so a half-local read would return the wrong value.
RWStructuredBuffer<uint> result : register(u0);
groupshared uint xcl_xchg[66];

[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    uint half = (gi >> 5) & 1u;
    uint lane = WaveGetLaneIndex() + 32u * half;
    uint base = (gi / 64u) * 66u;
    uint v = lane * 3u + 7u;              // distinct value per logical lane

    uint4 b = WaveActiveBallot(lane > 40u);
    if (WaveGetLaneIndex() == 0u) xcl_xchg[base + 64u + half] = b.x;
    GroupMemoryBarrierWithGroupSync();
    uint2 mask = uint2(xcl_xchg[base + 64u], xcl_xchg[base + 65u]);
    GroupMemoryBarrierWithGroupSync();

    // first_lane of the full 64-bit mask.
    uint first = (mask.x != 0u) ? firstbitlow(mask.x) : (32u + firstbitlow(mask.y));
    // xcl_xchg_rla: publish every lane's value, barrier, read the selected lane.
    xcl_xchg[base + lane] = v;
    GroupMemoryBarrierWithGroupSync();
    uint selected = xcl_xchg[base + first];
    GroupMemoryBarrierWithGroupSync();

    result[gi] = selected;                // lane 41 -> 41*3+7 = 130
}
