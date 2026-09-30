#include "line_common.hlsli"

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

StructuredBuffer<uint>      CurveBegins    : register(t1);
StructuredBuffer<float3>    control_points : register(t3); // native degree, packed: see Indices
StructuredBuffer<uint>      BezierIndexMap : register(t4);
StructuredBuffer<ColorData> colors         : register(t5);
StructuredBuffer<Indices>   indices        : register(t6);

// Bernstein form evaluated Horner-style, straight from the control points in [range.x, range.y):
// x0 carries C(n, k) * t^k from one term to the next and every step multiplies what came before by
// (1 - t), so after the last point value = sum C(n, k) (1-t)^(n-k) t^k P_k. Exact at both ends:
// t = 0 leaves P0, and t = 1 zeroes every term but the last, whose weight is exactly 1.
float3 Eval(float t, uint2 range) {
    float3 value = control_points[range.x];
    float x0 = 1.0;
    const uint count = range.y - range.x; // degree + 1
    for (uint k = 1u; k < count; ++k) {
        x0 *= t * float(count - k) / float(k);
        value = value * (1.0 - t) + x0 * control_points[range.x + k];
    }
    return value;
}

float3 SampleColor(uint4 color, float height, float t) {
    const float minHeight = asfloat(color.z);
    const float maxHeight = asfloat(color.w);
    float blend = t;
    if (minHeight < maxHeight)
        blend = saturate((height - minHeight) / (maxHeight - minHeight));
    return lerp(UnpackColorBits(color.x).rgb, UnpackColorBits(color.y).rgb, blend);
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
    const uint4 range = indices[curveIndex].first_last_first_bez_last_bez;
    const float t = float(sampleIndex - range.x) / float(range.y - range.x);
    const float3 p = Eval(t, range.zw);

    o.Position = mul(float4(p, 1.0), VP);
    o.Color = SampleColor(colors[curveIndex].c0_c1_height0_height1, p.y, t);
    return o;
}
