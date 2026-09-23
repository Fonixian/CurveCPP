#include "solid_common.hlsli"

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData     : register(t0);
StructuredBuffer<uint>            BezierIndexMap : register(t1);

RWStructuredBuffer<float4> CalculatedPoints : register(u0);

// Monomial base
float4 EvaluateBezier(float3 k0, float3 k1, float3 k2, float3 k3, float t) {
    return float4(mad(mad(mad(k3, t, k2), t, k1), t, k0), 1.0);
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint pointIndex = dispatchId.x;
    if (pointIndex >= TotalPointCount) return;

    uint curveIndex = BezierIndexMap[pointIndex];
    BezierCurveData bez = BezierData[curveIndex];

    int firstIndex = bez.FirstIndex;
    int lastIndex  = bez.LastIndex;

    float t = float(pointIndex - firstIndex) / float(lastIndex - firstIndex);
    float3 p0 = EvaluateBezier(bez.K0, bez.K1, bez.K2, bez.K3, t);

    // Color + Positon
    if (bez.MinHeight < bez.MaxHeight)
        t = saturate((p0.y - bez.MinHeight) / (bez.MaxHeight - bez.MinHeight));

    float4 colorBegin = UnpackColorBits(bez.ColorBegin);
    float4 colorEnd   = UnpackColorBits(bez.ColorEnd);
    float4 color = lerp(colorBegin, colorEnd, t);

    CalculatedPoints[pointIndex] = float4(p0, asfloat(PackColorBits(color)));
}