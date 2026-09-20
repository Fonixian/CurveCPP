#include "dot_common.hlsli"

// Pass 1 of 3 in the deterministic dot layout: count only, no offsets.
//
// This used to hand each curve its base offset with an InterlockedAdd on a
// shared counter, which made a curve's slice depend on the order the thread
// groups retired - the same scene could lay its dots out differently from
// one frame or one machine to the next. Now each curve only writes its own
// count and ParalellScan turns the counts into offsets (dot_resolve).

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData  : register(t0);
StructuredBuffer<float2>          Distances   : register(t1);
StructuredBuffer<DotStyle>        DotStyles   : register(t2);

// Scanned in place by ParalellScan straight after this pass, so the count has
// to survive somewhere else too - hence the second copy in DotRanges.y.
// That duplicate uint per curve is the whole memory cost of dropping the atomic.
RWStructuredBuffer<uint>  DotOffsets : register(u0);
RWStructuredBuffer<uint2> DotRanges  : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    float prevArcLength = Distances[BezierData[curveIndex].FirstIndex].x;
    float totalArcLength = Distances[BezierData[curveIndex].LastIndex].x;
    float curveSpacing = DotStyles[curveIndex].Spacing;

    uint dotCount_start = (prevArcLength > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(prevArcLength / curveSpacing) + 1u
        : 0u;
    
    uint dotCount_end = (totalArcLength > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(totalArcLength / curveSpacing) + 1u
        : 0u;

    DotOffsets[curveIndex] = dotCount_end - dotCount_start;

    // .x is left at 0 and filled in by dot_resolve once the scan has run.
    DotRanges[curveIndex] = uint2(0u, dotCount_end - dotCount_start);
}
