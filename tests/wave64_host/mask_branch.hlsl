// (a) v_cmp mask used for a whole-wave branch on two 32-lane subgroups.
// Synthetic RDNA2 body (mask_branch.s): v_cmp_gt_u32 vcc, 40, lane_id; s_cbranch_vccz.
// Each half publishes its 32-bit ballot and both read the full mask, so the branch
// decision is uniform. Without the sync, half 0 sees an empty mask and takes the
// wrong path.
RWStructuredBuffer<uint> result : register(u0);
groupshared uint xcl_xchg[66];

[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    uint half = (gi >> 5) & 1u;
    uint lane = WaveGetLaneIndex() + 32u * half;
    uint base = (gi / 64u) * 66u;

    uint4 b = WaveActiveBallot(lane > 40u);   // v_cmp_gt_u32 vcc, 40, lane_id
    if (WaveGetLaneIndex() == 0u) xcl_xchg[base + 64u + half] = b.x;
    GroupMemoryBarrierWithGroupSync();
    uint2 mask = uint2(xcl_xchg[base + 64u], xcl_xchg[base + 65u]);
    GroupMemoryBarrierWithGroupSync();

    // s_cbranch_vccz: both halves decide on (vcc_lo | vcc_hi).
    result[gi] = ((mask.x | mask.y) == 0u) ? 100u : 200u;
}
