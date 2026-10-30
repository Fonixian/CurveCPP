#include "dot_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint2>    CurveIndices   : register(t0);
StructuredBuffer<float2>   WorldDistances : register(t1); // cumulative, as (hi, lo) pairs
StructuredBuffer<DotStyle> CurveStyles    : register(t3);
StructuredBuffer<uint>     DotOffsets     : register(t4);

RWStructuredBuffer<DotSample> Dots : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount) return;

    uint capacity, stride;
    Dots.GetDimensions(capacity, stride);
    uint dotFirst = DotOffsets[curveIndex];
    uint dotCount = min(DotOffsets[curveIndex + 1u], capacity) - min(dotFirst, capacity);

    if (dotCount == 0) return;

    uint2 range          = CurveIndices[curveIndex];
    uint  sampleStartIdx = range.x;
    uint  sampleEndIdx   = range.y;
    float worldSpacing   = CurveStyles[curveIndex].spacing;

    // The first grid point this curve owns - the same exact mapping as dot_ini.
    uint dotBase = PairGridCount(WorldDistances[sampleStartIdx], worldSpacing);

    for (uint localIdx = threadLaneIdx; localIdx < dotCount; localIdx += 8)
    {
        uint  globalIdx       = dotFirst + localIdx;

        // The grid point k * spacing, exactly, as a pair.
        float2 target = PairFromProduct((float)(dotBase + localIdx), worldSpacing);

        uint low     = sampleStartIdx;
        uint high    = sampleEndIdx;
        uint sampleA = sampleStartIdx;

        while (low <= high) {
            uint mid = (low + high) / 2;
            if (PairLessEqual(WorldDistances[mid], target)) {
                sampleA = mid;
                low     = mid + 1;
            } else {
                if (mid == sampleStartIdx) break;
                high = mid - 1;
            }
        }

        uint sampleB = min(sampleA + 1, sampleEndIdx);

        // Taken pairwise, as precise as the segment itself rather than the arc it starts at.
        float2 distA = WorldDistances[sampleA];
        float segmentLength = PairDiff(WorldDistances[sampleB], distA);

        float segmentT = (segmentLength > 0.00001f)
            ? PairDiff(target, distA) / segmentLength
            : 0.0f;

        // The dot's curve parameter: the sample's own t plus the fraction of the way to the next one.
        // Samples are evenly spaced in t, so this is exact. At the curve's last sample sampleB ==
        // sampleA, segmentT is 0 and t is 1.
        DotSample dotSample;
        dotSample.T = (float(sampleA - sampleStartIdx) + saturate(segmentT)) / float(sampleEndIdx - sampleStartIdx);
        dotSample.CurveIndex = curveIndex;
        Dots[globalIdx] = dotSample;
    }
}
