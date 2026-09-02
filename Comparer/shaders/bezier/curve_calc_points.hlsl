#include "curve_common.hlsli"

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData     : register(t0);
StructuredBuffer<uint>            BezierIndexMap : register(t1);

RWStructuredBuffer<float4> CalculatedPoints : register(u0);
RWStructuredBuffer<float2> Distances        : register(u1);

float3 EvaluateBezier(float3 p0, float3 p1, float3 p2, float3 p3, float t) {
    float omt = 1.0 - t;
    float omt2 = omt * omt;
    float t2 = t * t;
    return omt2 * omt * p0
         + 3.0 * omt2 * t * p1
         + 3.0 * omt * t2 * p2
         + t2 * t * p3;
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint pointIndex = dispatchId.x;
    if (pointIndex >= TotalPointCount) return;

    uint curveIndex = BezierIndexMap[pointIndex];
    BezierCurveData bez = BezierData[curveIndex];

    uint firstIndex = (uint)bez.FirstIndex;
    uint lastIndex  = (uint)bez.LastIndex;
    uint resolution = lastIndex - firstIndex + 1;

    float t = float(pointIndex - firstIndex) / float(resolution - 1);
    float3 position = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, t);

    float dist = 0.0;
    float dist_screen = 0.0;
    if (pointIndex < lastIndex) {
        float tNext = float(pointIndex - firstIndex + 1) / float(resolution - 1);
        float3 nextPosition = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, tNext);
        dist = distance(position, nextPosition);

        float4 a = float4(position, 1.0);
        float4 b = float4(nextPosition, 1.0);

        a = mul(a, VP);
        b = mul(b, VP);

        a.xy /= a.w;
        b.xy /= b.w;

        float2 aScreen = (a.xy * 0.5 + 0.5) * WH;
        float2 bScreen = (b.xy * 0.5 + 0.5) * WH;
        dist_screen = distance(aScreen, bScreen);
    }
    Distances[pointIndex] = float2(dist, dist_screen);

    float blend = t;
    if (bez.MinHeight < bez.MaxHeight)
        blend = saturate((position.y - bez.MinHeight) / (bez.MaxHeight - bez.MinHeight));

    float4 colorBegin = UnpackColorBits(bez.ColorBegin);
    float4 colorEnd   = UnpackColorBits(bez.ColorEnd);
    float4 color = lerp(colorBegin, colorEnd, blend);

    CalculatedPoints[pointIndex] = float4(position, asfloat(PackColorBits(color)));
}
