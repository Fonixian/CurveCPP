// Step 3 of the curve pipeline: how many pattern centers does each curve carry, and where does its
// block live inside the one shared pattern buffer.
//
// Both answers depend on WORLD arc length and WORLD spacing only, so they survive camera moves and
// this pass runs on a curve change rather than every frame. The counter it accumulates into is read
// back on the CPU to size the pattern buffer.
#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData  : register(t0);
StructuredBuffer<float>           Distances   : register(t1); // Cumulative WORLD arc length per point
StructuredBuffer<CurveStyle>      CurveStyles : register(t2);

RWStructuredBuffer<uint>  PatternCounter : register(u0);
RWStructuredBuffer<uint2> PatternRanges  : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount)
        return;

    BezierCurveData bez = BezierData[curveIndex];
    uint lastIndex = (uint)bez.LastIndex;

    // The prefix sum is exclusive, so the last point of a curve holds that curve's total length.
    float totalArcLength = Distances[lastIndex];
    float curveSpacing = CurveStyles[curveIndex].Spacing;

    uint patternCount = (totalArcLength > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(totalArcLength / curveSpacing) + 1u
        : 0u;

    uint patternFirst;
    InterlockedAdd(PatternCounter[0], patternCount, patternFirst);
    PatternRanges[curveIndex] = uint2(patternFirst, patternCount);
}
