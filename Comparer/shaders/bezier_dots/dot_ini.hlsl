#include "dot_common.hlsli"

// Pass 1 of 2 in the deterministic dot layout: count only, no offsets.
//
// The count is written ONCE, into the buffer ParalellScan consumes. It does not survive the scan and
// does not need to: with the grand total appended one slot past the last curve, curve i's count is
// the gap between neighbouring offsets, the last curve included.

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData  : register(t0);
StructuredBuffer<float>           Distances   : register(t1);
StructuredBuffer<DotStyle>        DotStyles   : register(t2);

RWStructuredBuffer<uint> DotIndices : register(u0);

uint pattern_count(float arc, float spacing)
{
    return (arc > 0.0 && spacing > 0.0) ? (uint)floor(arc / spacing) + 1u : 0u;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    float prev_arc = Distances[BezierData[curveIndex].FirstIndex];
    float current_arc = Distances[BezierData[curveIndex].LastIndex];
    float spacing = DotStyles[curveIndex].Spacing;
    
    uint dot_count = pattern_count(current_arc, spacing) - pattern_count(prev_arc, spacing);
    
    DotIndices[curveIndex] = dot_count;
}
