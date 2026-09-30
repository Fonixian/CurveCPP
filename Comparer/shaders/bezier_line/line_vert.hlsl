#include "line_common.hlsli"

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

StructuredBuffer<uint>            CurveBegins    : register(t1);
StructuredBuffer<BezierCurveData> BezierData     : register(t3);
StructuredBuffer<uint>            BezierIndexMap : register(t4);
StructuredBuffer<float>           CurveWidths    : register(t6);

float3 EvaluateBezier(BezierCurveData bez, float t) {
    return mad(mad(mad(bez.K3, t, bez.K2), t, bez.K1), t, bez.K0);
}

float SampleT(BezierCurveData bez, uint sampleIndex) {
    return float(sampleIndex - (uint)bez.FirstIndex) / float((uint)bez.LastIndex - (uint)bez.FirstIndex);
}

float3 SampleColor(BezierCurveData bez, float height, float t) {
    float blend = t;
    if (bez.MinHeight < bez.MaxHeight)
        blend = saturate((height - bez.MinHeight) / (bez.MaxHeight - bez.MinHeight));
    return lerp(UnpackColorBits(bez.ColorBegin).rgb, UnpackColorBits(bez.ColorEnd).rgb, blend);
}

LineVSOutput main(uint vertexId : SV_VertexID) {
    LineVSOutput o;

    const uint sampleIndex = vertexId;
    const uint next = sampleIndex + 1u;

    const bool valid = next < TotalPointCount && ((CurveBegins[next >> 5u] >> (next & 31u)) & 1u) == 0u;

    const uint curveIndex = BezierIndexMap[sampleIndex];
    const BezierCurveData bez = BezierData[curveIndex];
    const float t = SampleT(bez, sampleIndex);
    const float3 p = EvaluateBezier(bez, t);

    o.Position = mul(float4(p, 1.0), VP);
    o.Color = SampleColor(bez, p.y, t);
    o.Style = float4(WH, CurveWidths[curveIndex], valid ? 1.0 : 0.0);
    return o;
}
