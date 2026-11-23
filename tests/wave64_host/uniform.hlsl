// (b) A readlane of a wave-uniform value is the invocation's own value.
// Synthetic RDNA2 body (uniform.s): v_mov_b32 v0, s0; v_readlane_b32 s1, v0, 32.
RWStructuredBuffer<uint> result : register(u0);

[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    uint uniform_value = 0xABCDEF01u;  // SGPR broadcast: same in all 64 lanes
    uint v = uniform_value;            // v_mov_b32 v0, s0
    result[gi] = v;                    // rewritten readlane: own value
}
