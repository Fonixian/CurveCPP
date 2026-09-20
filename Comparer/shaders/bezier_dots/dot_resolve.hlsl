#include "dot_common.hlsli"

// Pass 3 of 3 in the deterministic dot layout, after dot_ini has
// written the counts and ParalellScan has turned DotOffsets into an
// exclusive prefix sum of them.
//
// DotOffsets[i] is now the number of dots in curves 0..i-1,
// which is exactly curve i's base index into the flat dot array - the same
// number the InterlockedAdd used to produce, but decided by curve order rather
// than by scheduling. Nothing downstream changed: dot_ranges keeps the same
// uint2(first, count) layout dot_calc and dot_vert already read.

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint> DotOffsets : register(t0);

RWStructuredBuffer<uint>  DotCounter : register(u0);
RWStructuredBuffer<uint2> DotRanges  : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    uint dotFirst = DotOffsets[curveIndex];
    uint dotCount = DotRanges[curveIndex].y;

    DotRanges[curveIndex] = uint2(dotFirst, dotCount);

    // The scan is exclusive, so the last curve's base plus its own count is the
    // grand total. One ordinary store, no atomic, and the same value every run.
    if (curveIndex == TotalCurveCount - 1u) DotCounter[0] = dotFirst + dotCount;
}
