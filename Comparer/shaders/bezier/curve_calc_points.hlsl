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
// Segment lengths, one float per point, in two separate buffers - each gets its own segmented scan
// and every reader downstream wants one channel or the other, never both interleaved.
RWStructuredBuffer<float>  WorldDistances   : register(u1);
RWStructuredBuffer<float>  ScreenDistances  : register(u2);

// Horner over the monomial coefficients BezierCurveData carries (see curve_common.hlsli): three
// fused multiply-adds per component, against the Bernstein form's three weight products plus four
// scale-adds. Duplicated in solid_calc_points.hlsl and dot_common.hlsli on purpose, like the rest of
// the per-folder shader code - if you change the spelling here, change it there too, or the three
// renderers will place their samples a last-bit apart.
float3 EvaluateBezier(float3 k0, float3 k1, float3 k2, float3 k3, float t) {
    return mad(mad(mad(k3, t, k2), t, k1), t, k0);
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
    float3 position = EvaluateBezier(bez.K0, bez.K1, bez.K2, bez.K3, t);

    float dist = 0.0;
    float dist_screen = 0.0;
    if (pointIndex < lastIndex) {
        float tNext = float(pointIndex - firstIndex + 1) / float(resolution - 1);
        float3 nextPosition = EvaluateBezier(bez.K0, bez.K1, bez.K2, bez.K3, tNext);
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
    WorldDistances[pointIndex]  = dist;
    ScreenDistances[pointIndex] = dist_screen;

    float blend = t;
    if (bez.MinHeight < bez.MaxHeight)
        blend = saturate((position.y - bez.MinHeight) / (bez.MaxHeight - bez.MinHeight));

    float4 colorBegin = UnpackColorBits(bez.ColorBegin);
    float4 colorEnd   = UnpackColorBits(bez.ColorEnd);
    float4 color = lerp(colorBegin, colorEnd, blend);

    CalculatedPoints[pointIndex] = float4(position, asfloat(PackColorBits(color)));
}
