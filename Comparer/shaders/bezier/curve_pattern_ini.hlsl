#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount; // samples, 64 per piece
    uint     TotalCurveCount; // pieces
};

StructuredBuffer<float>        WorldDistances : register(t0); // arc at each sample, from the chain's start
StructuredBuffer<PatternStyle> CurveStyles    : register(t1);

RWStructuredBuffer<uint> PatternOffsets : register(u0);

uint pattern_count(float arc, float spacing) {
    return (arc > 0.0 && spacing > 0.0) ? (uint) floor(arc / spacing) + 1u : 0u;
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint pieceIndex = dispatchThreadId.x;
    if (pieceIndex >= TotalCurveCount) return;

    // The piece spans its first sample to its last; no range lookup, the block is fixed.
    uint firstSample = pieceIndex << PieceSampleShift;
    float prevArc = WorldDistances[firstSample];
    float currentArc = WorldDistances[firstSample + PieceSampleMask];
    float spacing = CurveStyles[pieceIndex].spacing;
    
    PatternOffsets[pieceIndex] = pattern_count(currentArc, spacing) - pattern_count(prevArc, spacing);
}
