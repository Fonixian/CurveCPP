#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData  : register(t0);
StructuredBuffer<float2>          Distances   : register(t1);
StructuredBuffer<CurveStyle>      CurveStyles : register(t2);

RWStructuredBuffer<uint>  PatternCounter : register(u0);
RWStructuredBuffer<uint2> PatternRanges  : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;
    
    // The prefix sum is exclusive, so the last point of a curve holds that curve's total length.
    float totalArcLength = Distances[BezierData[curveIndex].LastIndex].x;
    float curveSpacing = CurveStyles[curveIndex].Spacing;

    uint patternCount = (totalArcLength > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(totalArcLength / curveSpacing) + 1u
        : 0u;

    uint patternFirst;
    InterlockedAdd(PatternCounter[0], patternCount, patternFirst);
    PatternRanges[curveIndex] = uint2(patternFirst, patternCount);
}
