#include "dot_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData  : register(t0);
StructuredBuffer<float2>          Distances   : register(t1);
StructuredBuffer<DotStyle>        DotStyles   : register(t3);
StructuredBuffer<uint2>           DotRanges   : register(t4);

RWStructuredBuffer<DotSample> Dots : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount) return;

    uint2 range    = DotRanges[curveIndex];
    uint  dotFirst = range.x;
    uint  dotCount = range.y;

    if (dotCount == 0) return;

    BezierCurveData bez            = BezierData[curveIndex];
    uint            sampleStartIdx = (uint)bez.FirstIndex;
    uint            sampleEndIdx   = (uint)bez.LastIndex;
    float           worldSpacing   = DotStyles[curveIndex].Spacing;

    for (uint localIdx = threadLaneIdx; localIdx < dotCount; localIdx += 8)
    {
        uint  globalIdx       = dotFirst + localIdx;
        float targetWorldDist = (float)localIdx * worldSpacing;

        uint low     = sampleStartIdx;
        uint high    = sampleEndIdx;
        uint sampleA = sampleStartIdx;

        while (low <= high) {
            uint mid = (low + high) / 2;
            if (Distances[mid].x <= targetWorldDist) {
                sampleA = mid;
                low     = mid + 1;
            } else {
                if (mid == sampleStartIdx) break;
                high = mid - 1;
            }
        }

        uint sampleB = min(sampleA + 1, sampleEndIdx);

        float distA = Distances[sampleA].x;
        float distB = Distances[sampleB].x;
        float segmentLength = distB - distA;

        float segmentT = (segmentLength > 0.00001f)
            ? (targetWorldDist - distA) / segmentLength
            : 0.0f;

        DotSample dot;
        dot.SampleA = sampleA;
        dot.SampleB = sampleB;
        dot.SegmentT = saturate(segmentT);
        dot.Padding = 0.0;
        Dots[globalIdx] = dot;
    }
}
