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
StructuredBuffer<DotStyle>        DotStyles   : register(t2);

RWStructuredBuffer<uint>  DotCounter : register(u0);
RWStructuredBuffer<uint2> DotRanges  : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    float totalArcLength = Distances[BezierData[curveIndex].LastIndex].x;
    float curveSpacing = DotStyles[curveIndex].Spacing;

    uint dotCount = (totalArcLength > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(totalArcLength / curveSpacing) + 1u
        : 0u;

    uint dotFirst;
    InterlockedAdd(DotCounter[0], dotCount, dotFirst);
    DotRanges[curveIndex] = uint2(dotFirst, dotCount);
}
