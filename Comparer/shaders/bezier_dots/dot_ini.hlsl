#include "dot_common.hlsli"

// Dot count per curve. ParalellScan turns DotOffsets into each curve's base index in place, with the
// total appended one past the last curve.

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint2>    CurveIndices   : register(t0);
StructuredBuffer<float2>   WorldDistances : register(t1); // cumulative, as (hi, lo) pairs
StructuredBuffer<DotStyle> CurveStyles    : register(t2);

RWStructuredBuffer<uint> DotOffsets : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    uint2 range = CurveIndices[curveIndex];
    float spacing = CurveStyles[curveIndex].spacing;

    // The grid points k * spacing the curve's arc range owns - the same exact mapping dot_calc uses.
    uint dotCount = PairGridCount(WorldDistances[range.y], spacing) - PairGridCount(WorldDistances[range.x], spacing);

    DotOffsets[curveIndex] = dotCount;
}
