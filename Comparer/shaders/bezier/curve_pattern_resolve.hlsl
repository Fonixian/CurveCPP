#include "curve_common.hlsli"

// Pass 3 of 3 in the deterministic pattern layout, after curve_pattern_ini has
// written the counts and ParalellScan has turned PatternOffsets into an
// exclusive prefix sum of them.
//
// PatternOffsets[i] is now the number of pattern centres in curves 0..i-1,
// which is exactly curve i's base index into the flat pattern array - the same
// number the InterlockedAdd used to produce, but decided by curve order rather
// than by scheduling. Nothing downstream changed: pattern_ranges keeps the same
// uint2(first, count) layout curve_pattern_calc and curve_vs already read.

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint> PatternOffsets : register(t0);

RWStructuredBuffer<uint>  PatternCounter : register(u0);
RWStructuredBuffer<uint2> PatternRanges  : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    uint patternFirst = PatternOffsets[curveIndex];
    uint patternCount = PatternRanges[curveIndex].y;

    PatternRanges[curveIndex] = uint2(patternFirst, patternCount);

    // The scan is exclusive, so the last curve's base plus its own count is the
    // grand total. One ordinary store, no atomic, and the same value every run.
    if (curveIndex == TotalCurveCount - 1u) PatternCounter[0] = patternFirst + patternCount;
}
