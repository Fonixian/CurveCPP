#ifndef DOT_COMMON_HLSLI
#define DOT_COMMON_HLSLI

// Per-curve data, split by how often it changes - see BezierSplitRendererBase in bezier_common.h.
// The control points are a plain StructuredBuffer<float3>, four per curve at curveIndex * 4: the cubic
// as its four Bezier control points P0..P3 (lower degrees raised to cubic on upload), evaluated in
// Bernstein form by EvaluateBezier below. Same layout as line_common.hlsli / solid_common.hlsli, duplicated on purpose.
//
// There is no chain range here. The dot count and placement read the chain-global arc length at the
// curve's own first/last sample (the segmented scan runs across a whole chain), and a dot's two
// bracketing samples always belong to one curve, so nothing in this renderer looks across a joint.
struct ColorData {
    uint4 c0_c1_height0_height1; // ColorBegin, ColorEnd, asuint(MinHeight), asuint(MaxHeight)
};

struct Indices {
    uint2 first_last; // FirstIndex, LastIndex: the curve's sample range, both inclusive
};

struct DotStyle {
    // width << 24 | cap_front << 16 | cap_back << 8: the width (dot radius) in whole pixels [0, 255] in
    // the top byte, then the caps where the solid renderer's CapCapJoin keeps them. The low byte - the
    // join in the other renderers - is always 0, dots never join.
    uint  width_capcap;
    float spacing;     // world arc length between dot centres; <= 0 means no dots on this curve
};

// One dot: which two consecutive curve samples bracket it, and how far between them.
//
// CurveIndex is the curve those samples belong to. dot_calc has it in hand anyway and it fits in
// what used to be padding, so dot_vert can go straight to the per-curve buffers without the point ->
// curve index map - which matters now that dot_vert evaluates the curve itself rather than reading a
// position someone else stored.
struct DotSample {
    uint Sample;
    float SegmentT;
    uint CurveIndex;
};

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

float4 UnpackColorBits(uint packed) {
    uint4 unpacked_u = uint4(packed, packed, packed, packed);
    unpacked_u >>= uint4(0, 8, 16, 24);
    unpacked_u &= 0xFF;
    return float4(unpacked_u) / 255.0;
}

float DotRadius(uint width_capcap) { return float(width_capcap >> 24); }
uint  FrontCap(uint width_capcap)  { return (width_capcap >> 16) & 0xFF; }
uint  BackCap(uint width_capcap)   { return (width_capcap >> 8) & 0xFF; }

// The four control points of one curve. Each shader that evaluates the curve declares its own
// ControlPoints buffer (the slot differs between the point pass and the draw) and fills this with a
// four-load LoadCubic of its own; the loads are independent and go out together.
struct Cubic {
    float3 p0, p1, p2, p3;
};

// Shared by dot_calc_points and dot_vert. The point pass evaluates the curve to measure chord
// lengths and throws the positions away; dot_vert evaluates the same curve again at the two samples
// bracketing a dot. Keeping one copy of the polynomial here is what makes "again" mean "identically"
// - two spellings of a cubic that agree algebraically can still disagree in the last bit, and a dot
// placed a hair off the arc length it was counted from is exactly the kind of drift that shows up as
// a seam.
//
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

// Curve parameter of one sample index - resolution - 1 intervals span t in [0, 1]. `range` is the
// curve's inclusive sample range, Indices.first_last. The last sample is pinned to exactly 1: a GPU
// divide may be a reciprocal and a multiply, and n * (1 / n) is not always 1 in float (n = 41 is the
// first), which would leave P(1) a hair short of P3 and undo the exact endpoint EvaluateBezier gives.
float SampleT(uint2 range, uint sampleIndex) {
    return sampleIndex == range.y ? 1.0 : float(sampleIndex - range.x) / float(range.y - range.x);
}

// How far along the C0 -> C1 ramp a point sits: world Y clamped to the height band when one is set,
// otherwise the curve parameter itself.
float ColorBlend(uint4 color, float3 position, float t) {
    const float minHeight = asfloat(color.z);
    const float maxHeight = asfloat(color.w);
    if (minHeight < maxHeight)
        return saturate((position.y - minHeight) / (maxHeight - minHeight));
    return t;
}

#endif
