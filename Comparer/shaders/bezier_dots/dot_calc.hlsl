#include "dot_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint2>    CurveIndices   : register(t0);
StructuredBuffer<float>    WorldDistances : register(t1);
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

    float arcBegin = WorldDistances[sampleStartIdx];

    uint dotBase = (arcBegin > 0.0 && worldSpacing > 0.0)
        ? (uint) floor(arcBegin / worldSpacing) + 1u
        : 0u;

    for (uint localIdx = threadLaneIdx; localIdx < dotCount; localIdx += 8)
    {
        uint  globalIdx       = dotFirst + localIdx;

        float targetWorldDist = (float)(dotBase + localIdx) * worldSpacing;

        uint low     = sampleStartIdx;
        uint high    = sampleEndIdx;
        uint sampleA = sampleStartIdx;

        while (low <= high) {
            uint mid = (low + high) / 2;
            if (WorldDistances[mid] <= targetWorldDist) {
                sampleA = mid;
                low     = mid + 1;
            } else {
                if (mid == sampleStartIdx) break;
                high = mid - 1;
            }
        }

        uint sampleB = min(sampleA + 1, sampleEndIdx);

        float distA = WorldDistances[sampleA];
        float distB = WorldDistances[sampleB];
        float segmentLength = distB - distA;

        float segmentT = (segmentLength > 0.00001f)
            ? (targetWorldDist - distA) / segmentLength
            : 0.0f;

        DotSample dotSample;
        dotSample.Sample = sampleA;
        dotSample.SegmentT = saturate(segmentT);
        dotSample.CurveIndex = curveIndex;
        Dots[globalIdx] = dotSample;
    }
}
