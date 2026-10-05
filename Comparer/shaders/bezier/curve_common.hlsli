#ifndef CURVE_COMMON_HLSLI
#define CURVE_COMMON_HLSLI

// Per-curve data, split by how often it changes - see BezierSplitRendererBase in bezier_common.h.
// The control points are a plain StructuredBuffer<float3>, four per curve at curveIndex * 4: the cubic
// as its four Bezier control points P0..P3 (lower degrees raised to cubic on upload), evaluated in
// Bernstein form by EvaluateBezier below. Same layout as solid_common.hlsli / dot_common.hlsli / line_common.hlsli, duplicated on purpose.
//
// There is no chain SAMPLE range. The terminus test is per segment (Neighbors, from the begin bits,
// the way the solid renderer does it), and every other consumer - the point pass, pattern_ini/calc,
// the pattern coordinate - reads the chain-cumulative arc length at the curve's OWN first/last
// sample. The one chain-wide thing is curve_vs's CurveChains (first/last CURVE of the chain), which
// only bounds the slots the pixel shader may read - see PatternSlots below.
struct ColorData {
    uint4 c0_c1_height0_height1; // ColorBegin, ColorEnd, asuint(MinHeight), asuint(MaxHeight)
};

struct Indices {
    uint2 first_last; // FirstIndex, LastIndex: the curve's sample range, both inclusive
};

struct PatternStyle {
    // width << 24 | cap_front << 16 | cap_back << 8 | join: width in whole pixels [0, 255] in the top
    // byte, then the same CapCapJoin the solid renderer packs - FrontCap/BackCap/Join read it as-is.
    uint  width_capcapjoin;
    float spacing;      // world arc length between dash centres; <= 0 is solid
    float dash_length;  // one dash, cap to cap, in pixels
};

float StyleWidth(uint width_capcapjoin) { return float(width_capcapjoin >> 24); }

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

static const uint CurveJoinRound  = 0u;
static const uint CurveJoinSquare = 1u;

// Bit 24 of the CapCapJoin INTERPOLANT, set by the vertex shader. In the style buffer the top byte
// is the width; the vertex shader masks it off (the pixel shader has the width in SDF.z already)
// before setting this bit.
// Solid is Spacing <= 0 and nothing else, and the pixel shader no longer receives Spacing, so the
// test has to travel as a flag. Written from `Spacing > 0.0` so a NaN spacing reads as solid,
// matching the `!(spacing > 0.0)` it replaces.
static const uint CurvePatternedBit = 1u << 24;

struct CurveVSOutput {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;
    // rgb: stroke colour. w: the PATTERN COORDINATE - the chain's centre grid measured directly in
    // slots of PatternPosition, so it floor()s straight to a slot. The pixel shader used to build
    // that out of three interpolants:
    //
    //     slot = clamp(floor(TotalDistance / Spacing) + PatternSlot.x, PatternSlot.y, PatternSlot.z)
    //
    // TotalDistance, Spacing and PatternSlot.x collapse into this one number (see curve_vs.hlsl),
    // and it rides in the alpha slot the pixel shader has always discarded - it writes its own from
    // the SDF. PatternSlot.y/.z go with the per-curve window, see curve_ps.hlsl.
    noperspective float4 ColorPattern : COLOR0;
    nointerpolation float ScreenArcBegin : TEXCOORD1;
    // Whether the segment has a neighbour before B (x) and after C (y), from the begin bits - the
    // terminus test, exactly as in the solid renderer. It replaced ScreenArcEnd, which needed the
    // chain's last sample index: the pixel shader tested currentArc > ScreenArcEnd for the back cap.
    nointerpolation uint2 Neighbors : TEXCOORD2;
    nointerpolation float DashLength : TEXCOORD3;
    nointerpolation uint CapCapJoin : TEXCOORD4;
    // Signed tan(theta/2) of the screen-space turn at each joint: x at B (SDF.y == 0), y at C
    // (SDF.y == l_CB). 0 where there is no neighbour. Turns the segment-local arc into the arc
    // measured against the joint's angle bisector - see CurvePatternArc in curve_ps.hlsl.
    nointerpolation float2 ArcShear : TEXCOORD5;
    // The slots of PatternPosition this segment's CHAIN owns, [x, y): PatternOffsets of the chain's
    // first curve and of the curve just past its last. The flat array is one sorted run per chain,
    // each starting again at screen arc 0, so a slot outside this range is another stroke's centre
    // and must never be read. x == y for a patterned chain with no centre at all (all gap).
    nointerpolation uint2 PatternSlots : TEXCOORD6;
};

float4 UnpackColorBits(uint packed) {
    uint4 unpacked_u = uint4(packed, packed, packed, packed);
    unpacked_u >>= uint4(0, 8, 16, 24);
    unpacked_u &= 0xFF;
    return float4(unpacked_u) / 255.0;
}

// The four control points of one curve. The point pass and the vertex shader each declare their own
// ControlPoints buffer and a four-load LoadCubic; both evaluate through the ONE EvaluateBezier below,
// so a sample the point pass measured is the same float the vertex shader draws.
struct Cubic {
    float3 p0, p1, p2, p3;
};

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

// Colour of one sample: world Y clamped to the height band when one is set, otherwise t.
float3 SampleColor(uint4 color, float height, float t) {
    const float minHeight = asfloat(color.z);
    const float maxHeight = asfloat(color.w);
    float blend = t;
    if (minHeight < maxHeight)
        blend = saturate((height - minHeight) / (maxHeight - minHeight));
    return lerp(UnpackColorBits(color.x).rgb, UnpackColorBits(color.y).rgb, blend);
}

uint FrontCap(uint capcapjoin) { return (capcapjoin >> 16) & 0xFF; }
uint BackCap(uint capcapjoin) { return (capcapjoin >> 8) & 0xFF; }
uint Join(uint capcapjoin) { return capcapjoin & 0xFF; }
bool IsPatterned(uint capcapjoin) { return (capcapjoin & CurvePatternedBit) != 0u; }

#endif
