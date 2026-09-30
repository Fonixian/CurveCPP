#ifndef DOT_COMMON_HLSLI
#define DOT_COMMON_HLSLI

// Per-curve data, split by how often it changes - see BezierSplitRendererBase in bezier_common.h.
// The control points are a plain StructuredBuffer<float3>, four per curve at curveIndex * 4: the cubic
// in MONOMIAL form K0..K3, P(t) = K0 + t(K1 + t(K2 + tK3)) - NOT its control points, so do not read K1
// expecting P1. Same layout as line_common.hlsli / solid_common.hlsli, duplicated on purpose.
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

// The four coefficients of one curve. Each shader that evaluates the curve declares its own
// ControlPoints buffer (the slot differs between the point pass and the draw) and fills this with a
// four-load LoadCubic of its own; the loads are independent and go out together.
struct Cubic {
    float3 k0, k1, k2, k3;
};

// Shared by dot_calc_points and dot_vert. The point pass evaluates the curve to measure chord
// lengths and throws the positions away; dot_vert evaluates the same curve again at the two samples
// bracketing a dot. Keeping one copy of the polynomial here is what makes "again" mean "identically"
// - two spellings of a cubic that agree algebraically can still disagree in the last bit, and a dot
// placed a hair off the arc length it was counted from is exactly the kind of drift that shows up as
// a seam.
//
// Horner over the monomial coefficients: three fused multiply-adds per component, against the
// Bernstein form's three weight products plus four scale-adds. The coefficients arrive that way
// already (see the top of this file), so this is the whole evaluation.
float3 EvaluateBezier(Cubic c, float t)
{
    return mad(mad(mad(c.k3, t, c.k2), t, c.k1), t, c.k0);
}

// Curve parameter of one sample index. resolution - 1 intervals span t in [0, 1]. `range` is the
// curve's inclusive sample range, Indices.first_last.
float SampleT(uint2 range, uint sampleIndex) {
    return float(sampleIndex - range.x) / float(range.y - range.x);
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
