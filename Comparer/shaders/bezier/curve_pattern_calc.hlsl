// Step 4 of the curve pipeline: the screen-space arc length of every pattern center.
//
// Pattern k of a curve sits at k * Spacing in WORLD arc length - a camera-independent anchor, which
// is the whole point of the design. Screen arc length is not proportional to world arc length, so
// the world position is located by binary search in the world prefix sum and the same interpolation
// factor is then applied to the screen prefix sum.
#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData      : register(t0);
StructuredBuffer<float>           Distances       : register(t1); // Cumulative WORLD arc length per point
StructuredBuffer<float>           DistancesScreen : register(t2); // Cumulative SCREEN arc length per point
StructuredBuffer<CurveStyle>      CurveStyles     : register(t3);
StructuredBuffer<uint2>           PatternRanges   : register(t4); // Per curve: x = first, y = count

RWStructuredBuffer<float> PatternPosition : register(u0); // Screen arc length per pattern center

[numthreads(8, 8, 1)] // 8 threads horizontally per curve, 8 curves vertically per thread group
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount)
        return;

    uint2 range        = PatternRanges[curveIndex];
    uint  patternFirst = range.x;
    uint  patternCount = range.y;

    if (patternCount == 0)
        return;

    BezierCurveData bez            = BezierData[curveIndex];
    uint            sampleStartIdx = (uint)bez.FirstIndex;
    uint            sampleEndIdx   = (uint)bez.LastIndex;
    float           worldSpacing   = CurveStyles[curveIndex].Spacing;

    // Stride loop: spread an arbitrary number of pattern centers over the 8 lanes.
    for (uint localIdx = threadLaneIdx; localIdx < patternCount; localIdx += 8)
    {
        uint  globalIdx       = patternFirst + localIdx;
        float targetWorldDist = (float)localIdx * worldSpacing;

        // Binary search the world prefix sum for the segment [sampleA, sampleB] containing it.
        uint low     = sampleStartIdx;
        uint high    = sampleEndIdx;
        uint sampleA = sampleStartIdx;

        while (low <= high)
        {
            uint mid = (low + high) / 2;
            if (Distances[mid] <= targetWorldDist)
            {
                sampleA = mid;
                low     = mid + 1;
            }
            else
            {
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

        // Same factor, screen prefix sum.
        float screenDistA = DistancesScreen[sampleA];
        float screenDistB = DistancesScreen[sampleB];

        PatternPosition[globalIdx] = lerp(screenDistA, screenDistB, saturate(segmentT));
    }
}
