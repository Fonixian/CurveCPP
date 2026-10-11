#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint2>        CurveIndices   : register(t0);
StructuredBuffer<float>        WorldDistances : register(t1);
StructuredBuffer<PatternStyle> CurveStyles    : register(t2);

RWStructuredBuffer<uint> PatternOffsets : register(u0);

uint pattern_count(float arc, float spacing) {
    return (arc > 0.0 && spacing > 0.0) ? (uint) floor(arc / spacing) + 1u : 0u;
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    uint2 range = CurveIndices[curveIndex];
    float prevArc = WorldDistances[range.x];
    float currentArc = WorldDistances[range.y];
    float spacing = CurveStyles[curveIndex].spacing;
    
    uint dotCount = pattern_count(currentArc, spacing) - pattern_count(prevArc, spacing);

    PatternOffsets[curveIndex] = dotCount;
}
