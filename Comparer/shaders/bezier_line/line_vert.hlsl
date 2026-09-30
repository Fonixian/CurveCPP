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

// Drawn as a LINELIST of 2 * (TotalPointCount - 1) vertices: line `segment` joins sample `segment` to
// sample `segment + 1`, and each vertex evaluates the one sample it lands on. The segments that would
// bridge the last sample of one chain to the first sample of the next (the next sample carries a
// CurveBegins bit) are thrown away by sending BOTH of their vertices to the same point outside the
// clip volume, where the clipper rejects the whole line. That is the one job the old geometry shader
// did that the rasteriser cannot; near-plane clipping it now does itself.
LineVSOutput main(uint vertexId : SV_VertexID) {
    LineVSOutput o;

    const uint segment = vertexId >> 1u;
    const uint next = segment + 1u;
    const bool bridge = next >= TotalPointCount || ((CurveBegins[next >> 5u] >> (next & 31u)) & 1u) != 0u;

    if (bridge) {
        o.Position = float4(2.0, 2.0, 2.0, 1.0);
        o.Color = 0.0;
        return o;
    }

    const uint sampleIndex = segment + (vertexId & 1u);
    const uint curveIndex = BezierIndexMap[sampleIndex];
    const BezierCurveData bez = BezierData[curveIndex];
    const float t = SampleT(bez, sampleIndex);
    const float3 p = EvaluateBezier(bez, t);

    o.Position = mul(float4(p, 1.0), VP);
    o.Color = SampleColor(bez, p.y, t);
    return o;
}
