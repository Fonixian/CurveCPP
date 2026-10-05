#include "line_common.hlsli"

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

StructuredBuffer<uint>      CurveBegins    : register(t1);
StructuredBuffer<float3>    control_points : register(t3); // P0,P1,P2,P3; P0,P1,P2,P3; ...
StructuredBuffer<uint>      BezierIndexMap : register(t4);
StructuredBuffer<ColorData> colors         : register(t5);
StructuredBuffer<Indices>   indices        : register(t6);

// The cubic's control points P0..P3 sit at a fixed stride of four, so where to read them follows
// from the curve index alone - no lookup into `indices` has to come back first. All four loads are
// independent and go out together.
struct Cubic {
    float3 p0, p1, p2, p3;
};

Cubic LoadCubic(uint curveIndex) {
    const uint k = curveIndex << 2u;
    Cubic c;
    c.p0 = control_points[k];
    c.p1 = control_points[k + 1u];
    c.p2 = control_points[k + 2u];
    c.p3 = control_points[k + 3u];
    return c;
}

// Bernstein form, straight over the control points:
//
//     P(t) = s^3 P0 + 3 s^2 t P1 + 3 s t^2 P2 + t^3 P3,    s = 1 - t
//
// The weights come out as exactly (1, 0, 0, 0) at t = 0 and (0, 0, 0, 1) at t = 1, so the curve
// reaches P0 and P3 bit-exactly, in whatever order the compiler sums the terms. That is what makes a
// merged joint exact: the curve ending there evaluates P(1) = its P3, the one starting there P(0) = its
// P0, and those are the same control point. (The monomial form K0 + K1 + K2 + K3 only got within a few
// ulps of P3.) Cost: ~9 scalar ops for the weights plus a mul and three mads per component, against
// Horner's three mads per component; the loads are the same four float3.
float3 EvaluateBezier(Cubic c, float t) {
    const float s   = 1.0 - t;
    const float st3 = 3.0 * s * t;
    return mad(c.p3, t * t * t, mad(c.p2, st3 * t, mad(c.p1, st3 * s, c.p0 * (s * s * s))));
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
    const uint2 range = indices[curveIndex].first_last;
    const float t = float(sampleIndex - range.x) / float(range.y - range.x);
    const float3 p = EvaluateBezier(LoadCubic(curveIndex), t);

    o.Position = mul(float4(p, 1.0), VP);
    o.Color = SampleColor(colors[curveIndex].c0_c1_height0_height1, p.y, t);
    return o;
}
