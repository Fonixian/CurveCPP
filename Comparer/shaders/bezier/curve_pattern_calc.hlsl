#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint2>           CurveIndices    : register(t0);
StructuredBuffer<float2>          Distances       : register(t1); // x: cumulative world arc, y: cumulative screen arc
StructuredBuffer<PatternStyle>    CurveStyles     : register(t3);
StructuredBuffer<uint>            PatternOffsets  : register(t4);

RWStructuredBuffer<float> PatternPosition : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount) return;

    uint capacity, stride;
    PatternPosition.GetDimensions(capacity, stride);
    uint patternFirst = PatternOffsets[curveIndex];
    uint patternCount = min(PatternOffsets[curveIndex + 1u], capacity) - min(patternFirst, capacity);

    if (patternCount == 0) return;

    uint2 range          = CurveIndices[curveIndex];
    uint  sampleStartIdx = range.x;
    uint  sampleEndIdx   = range.y;
    float worldSpacing   = CurveStyles[curveIndex].spacing;

    float arcBegin = Distances[sampleStartIdx].x;

    uint patternBase = (arcBegin > 0.0 && worldSpacing > 0.0)
        ? (uint) floor(arcBegin / worldSpacing) + 1u
        : 0u;

    for (uint localIdx = threadLaneIdx; localIdx < patternCount; localIdx += 8)
    {
        uint  globalIdx       = patternFirst + localIdx;

        float targetWorldDist = (float)(patternBase + localIdx) * worldSpacing;

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

        // One 8-byte load per end gives both channels.
        float2 distA = Distances[sampleA];
        float2 distB = Distances[sampleB];
        float segmentLength = distB.x - distA.x;

        float segmentT = (segmentLength > 0.00001f)
            ? (targetWorldDist - distA.x) / segmentLength
            : 0.0f;

        PatternPosition[globalIdx] = lerp(distA.y, distB.y, saturate(segmentT));
    }
}
