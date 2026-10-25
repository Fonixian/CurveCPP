#include "line_common.hlsli"

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

// One row per coordinate, one column per control point: row x = (P0.x, P1.x, P2.x, P3.x), likewise y
// and z, so P3 sits in the .w of every row. row_major so the rows sit in memory in that order, matching
// the CPU's XMFLOAT3X4 whatever the compiler's default matrix packing is. The whole cubic is one 48-byte
// element found from the curve index alone - no lookup into `indices` has to come back first.
struct Cubic {
    row_major float3x4 P;
};

StructuredBuffer<Cubic>     control_points : register(t3); // one 3x4 per curve, rows x/y/z, columns P0..P3
StructuredBuffer<uint>      SegmentMap     : register(t4); // per segment: curve | SegmentHasPrev | SegmentHasNext
StructuredBuffer<ColorData> colors         : register(t5);
StructuredBuffer<Indices>   indices        : register(t6);

Cubic LoadCubic(uint curveIndex) {
    return control_points[curveIndex];
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
// Horner's three mads per component; the load is one 48-byte matrix (three dot4s).
float3 EvaluateBezier(Cubic c, float t) {
    const float s   = 1.0 - t;
    const float st3 = 3.0 * s * t;
    return mul(c.P, float4(s * s * s, st3 * s, st3 * t, t * t * t));
}

float3 SampleColor(uint4 color, float height, float t) {
    const float minHeight = asfloat(color.z);
    const float maxHeight = asfloat(color.w);
    float blend = t;
    if (minHeight < maxHeight)
        blend = saturate((height - minHeight) / (maxHeight - minHeight));
    return lerp(UnpackColorBits(color.x).rgb, UnpackColorBits(color.y).rgb, blend);
}

// Drawn as a LINELIST of 2 * total_segments vertices, one line per segment_map entry, i.e. per real
// segment: there is no line for the step from one curve to the next (between two chains it would
// bridge two strokes, inside a merged chain it would join the joint's two copies), so nothing has to
// be thrown away. Both ends of a line lie on its segment's curve - every curve has one segment fewer
// than samples, so segment s of curve c runs from sample s + c to s + c + 1 - and each vertex
// evaluates its own end of it. Near-plane clipping the rasteriser does itself.
LineVSOutput main(uint vertexId : SV_VertexID) {
    LineVSOutput o;

    const uint segment = vertexId >> 1u;
    const uint curveIndex = SegmentMap[segment] & SegmentCurveMask;
    const uint sampleIndex = segment + curveIndex + (vertexId & 1u);
    const uint2 range = indices[curveIndex].first_last;

    const float t = float(sampleIndex - range.x) / float(range.y - range.x);
    const float3 p = EvaluateBezier(LoadCubic(curveIndex), t);

    o.Position = mul(float4(p, 1.0), VP);
    o.Color = SampleColor(colors[curveIndex].c0_c1_height0_height1, p.y, t);
    return o;
}
