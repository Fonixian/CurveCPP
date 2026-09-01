struct BezierCurveData
{
    float3 P0;
    int    FirstIndex;
    float3 P1;
    int    LastIndex;
    float3 P2;
    int    ColorBegin;
    float3 P3;
    int    ColorEnd;
};

cbuffer DispatchInfo : register(b0)
{
    uint TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData : register(t0);
StructuredBuffer<float> Distances : register(t1);
StructuredBuffer<float> Spacing   : register(t2);

RWStructuredBuffer<uint>  DotCounter : register(u0);
RWStructuredBuffer<uint2> DotRanges  : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount)
        return;

    BezierCurveData bez = BezierData[curveIndex];
    
    uint lastIndex = (uint)bez.LastIndex;

    float totalArcLength = Distances[lastIndex];
    float curveSpacing = Spacing[curveIndex];

    uint dotCount = (totalArcLength > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(totalArcLength / curveSpacing) + 1u
        : 0u;

    uint dotFirst;
    InterlockedAdd(DotCounter[0], dotCount, dotFirst);
    DotRanges[curveIndex] = uint2(dotFirst, dotCount);
}