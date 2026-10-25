// Carry pass for curve_calc_points.hlsl's fused top-level scan: adds each block's scanned total (the
// arc length of the blocks in front of it, within the same segment) to the points that have no begin
// bit in front of them inside their block. Same thread layout as curve_calc_points.hlsl - 256 threads x
// 4 points, one block per group - so NeedsCarry is read once per thread and BlockSums once per group.
//
// Run only when the point pass had more than one block, after the lower levels have scanned BlockSums.
// Dispatch GroupCount(TotalPointCount, 1024).

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<float2>   BlockSums  : register(t0); // scanned: exclusive per block
StructuredBuffer<uint>     NeedsCarry : register(t1); // one per thread (per 4 points)

RWStructuredBuffer<float2> Distances  : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 dispatchId : SV_DispatchThreadID) {
    const uint base = dispatchId.x << 2u;
    if (base >= TotalPointCount) return;
    if (NeedsCarry[dispatchId.x] == 0u) return;

    const float2 carry = BlockSums[groupId.x];
    [unroll]
    for (uint j = 0u; j < 4u; ++j) {
        Distances[base + j] += carry;
    }
}
