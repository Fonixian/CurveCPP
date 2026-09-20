#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

// How many entries PatternPosition actually holds. The CPU sizes it from an upper bound that cannot
// be smaller than the count curve_pattern_ini arrives at, so the clamp below should never bite - it
// is here so a wrong bound loses the tail of a pattern instead of writing past the end.
cbuffer PatternCapacity : register(b1)
{
    uint  Capacity;
    uint3 CapacityPadding;
};

StructuredBuffer<BezierCurveData> BezierData      : register(t0);
StructuredBuffer<float2>          Distances       : register(t1);
StructuredBuffer<CurveStyle>      CurveStyles     : register(t3);
StructuredBuffer<uint2>           PatternRanges   : register(t4);

RWStructuredBuffer<float> PatternPosition : register(u0);

[numthreads(8, 32, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount) return;

    uint2 range        = PatternRanges[curveIndex];
    uint  patternFirst = range.x;
    uint  patternCount = range.y;

    if (patternCount == 0) return;

    BezierCurveData bez            = BezierData[curveIndex];
    uint            sampleStartIdx = (uint)bez.FirstIndex;
    uint            sampleEndIdx   = (uint)bez.LastIndex;
    float           worldSpacing   = CurveStyles[curveIndex].Spacing;

    // The centre grid is CHAIN-global (see curve_pattern_ini.hlsl): centre n sits at world distance
    // n * spacing from the chain's origin, not from this curve's own start. patternBase is the first
    // n this curve owns, recomputed from the same two numbers ini counted from, so slot
    // patternFirst + localIdx always holds grid index patternBase + localIdx.
    float arcBegin = Distances[sampleStartIdx].x;

    uint patternBase = (arcBegin > 0.0 && worldSpacing > 0.0)
        ? (uint) floor(arcBegin / worldSpacing) + 1u
        : 0u;

    for (uint localIdx = threadLaneIdx; localIdx < patternCount; localIdx += 8)
    {
        uint  globalIdx       = patternFirst + localIdx;
        if (globalIdx >= Capacity) continue;

        // One multiply from the grid index, so this lands on exactly the distance curve_ps.hlsl
        // inverts with floor(totalDistance / spacing) - accumulating base * spacing separately would
        // not. Every target is inside (arcBegin, arcEnd] by construction, so the search below always
        // brackets it properly instead of clamping to an end sample.
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

        float distA = Distances[sampleA].x;
        float distB = Distances[sampleB].x;
        float segmentLength = distB - distA;

        float segmentT = (segmentLength > 0.00001f)
            ? (targetWorldDist - distA) / segmentLength
            : 0.0f;

        float screenDistA = Distances[sampleA].y;
        float screenDistB = Distances[sampleB].y;

        PatternPosition[globalIdx] = lerp(screenDistA, screenDistB, saturate(segmentT));
    }
}
