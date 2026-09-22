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
RWStructuredBuffer<float>  WorldDistances   : register(u1);
RWStructuredBuffer<float>  ScreenDistances  : register(u2);

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

    uint firstIndex = (uint)bez.FirstIndex;
    uint lastIndex  = (uint)bez.LastIndex;
    uint resolution = lastIndex - firstIndex;
    uint i = pointIndex - firstIndex;
    float2 t = float2(uint2(i, min(i + 1, resolution))) / float(resolution);
    float4 p0 = EvaluateBezier(bez.K0, bez.K1, bez.K2, bez.K3, t.x);
    float4 p1 = EvaluateBezier(bez.K0, bez.K1, bez.K2, bez.K3, t.y);
    
    // Color + Position
    if (bez.MinHeight < bez.MaxHeight)
        t = saturate((p0.y - bez.MinHeight) / (bez.MaxHeight - bez.MinHeight));
    float4 colorBegin = UnpackColorBits(bez.ColorBegin);
    float4 colorEnd   = UnpackColorBits(bez.ColorEnd);
    float4 color = lerp(colorBegin, colorEnd, t.x);

    CalculatedPoints[pointIndex] = float4(p0.xyz, asfloat(PackColorBits(color)));
    
    // World Distance
    WorldDistances[pointIndex] = distance(p0, p1);
    
    // Screen Distance
    p0 = mul(p0, VP);
    p1 = mul(p1, VP);
    
    float4 screen = mad(float4(p0.xy / p0.w, p1.xy / p1.w), 0.5, 0.5) * float4(WH, WH);
    ScreenDistances[pointIndex] = distance(screen.xy, screen.zw);
}
