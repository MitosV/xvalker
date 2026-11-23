// (c) Cross-half lane exchange through groupshared memory in a workgroup.
// Synthetic RDNA2 body (exchange.s): ds_bpermute_b32 / v_readlane_b32 of lane 32+k.
RWStructuredBuffer<uint> result : register(u0);
groupshared uint xcl_xchg[64];

[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    xcl_xchg[gi] = gi;
    GroupMemoryBarrierWithGroupSync();
    uint other = xcl_xchg[gi ^ 32u]; // xcl_xchg_rla: lane of the other half
    GroupMemoryBarrierWithGroupSync();
    result[gi] = other;
}
