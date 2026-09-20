#include "dot_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

// How many entries Dots actually holds. The CPU sizes it from an upper bound that cannot be smaller
// than the count dot_ini arrives at, and dot_args caps the instance count to the same number, so the
// clamp below should never bite - it is here so a wrong bound loses the tail of the dots instead of
// writing past the end.
cbuffer DotCapacity : register(b1)
{
    uint  Capacity;
    uint3 CapacityPadding;
};

StructuredBuffer<BezierCurveData> BezierData  : register(t0);
StructuredBuffer<float>           Distances   : register(t1);
StructuredBuffer<DotStyle>        DotStyles   : register(t3);
// Exclusive scan of dot_ini's per-curve counts, with the grand total appended one slot past the
// last curve. DotOffsets[i] is curve i's base index into Dots and the gap to DotOffsets[i + 1] is
// its count - the last curve included, which is what the appended total buys.
StructuredBuffer<uint>            DotOffsets  : register(t4);

RWStructuredBuffer<DotSample> Dots : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount) return;

    uint dotFirst = DotOffsets[curveIndex];
    uint dotCount = DotOffsets[curveIndex + 1u] - dotFirst;

    if (dotCount == 0) return;

    BezierCurveData bez            = BezierData[curveIndex];
    uint            sampleStartIdx = (uint)bez.FirstIndex;
    uint            sampleEndIdx   = (uint)bez.LastIndex;
    float           worldSpacing   = DotStyles[curveIndex].Spacing;
    
    float prevArcLength = Distances[BezierData[curveIndex].FirstIndex];
    float curveSpacing = DotStyles[curveIndex].Spacing;

    uint base_index = (prevArcLength > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(prevArcLength / curveSpacing) + 1u
        : 0u;

    for (uint localIdx = threadLaneIdx; localIdx < dotCount; localIdx += 8)
    {
        uint  globalIdx       = dotFirst + localIdx;
        if (globalIdx >= Capacity) continue;

        float targetWorldDist = (float) localIdx * worldSpacing + worldSpacing * (float)base_index;

        uint low     = sampleStartIdx;
        uint high    = sampleEndIdx;
        uint sampleA = sampleStartIdx;

        while (low <= high) {
            uint mid = (low + high) / 2;
            if (Distances[mid] <= targetWorldDist) {
                sampleA = mid;
                low     = mid + 1;
            } else {
                if (mid == sampleStartIdx) break;
                high = mid - 1;
            }
        }

        uint sampleB = min(sampleA + 1, sampleEndIdx);

        float distA = Distances[sampleA];
        float distB = Distances[sampleB];
        float segmentLength = distB - distA;

        float segmentT = (segmentLength > 0.00001f)
            ? (targetWorldDist - distA) / segmentLength
            : 0.0f;

        DotSample dot;
        dot.SampleA = sampleA;
        dot.SampleB = sampleB;
        dot.SegmentT = saturate(segmentT);
        // Carried so dot_vert can reach BezierData without the point -> curve index map.
        dot.CurveIndex = curveIndex;
        Dots[globalIdx] = dot;
    }
}
