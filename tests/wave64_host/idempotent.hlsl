// (a) Idempotent wave64 reduction on two 32-lane subgroups.
// Synthetic RDNA2 body (idempotent.s): min chain + v_readlane_b32 ..., 32.
// The emitter rewrites the read of lane 32+k to lane k of the invoking half
// (rla_local), so each half folds its own lanes with WaveActiveMin.
RWStructuredBuffer<uint> result : register(u0);

[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    uint v = gi;                       // v_mbcnt_lo/hi: distinct per lane
    uint own = WaveActiveMin(v);       // dpp8 + row_mirror + permlanex16
    uint other = WaveReadLaneAt(own, 32u & 31u); // rla_local(v, lane & 31)
    result[gi] = min(own, other);
}
