#ifndef CURVE_COMMON_HLSLI
#define CURVE_COMMON_HLSLI

#include "../arc_pair.hlsli"

// Distances, one per sample, as the segmented scan leaves them: the arc length from the chain's start,
// world and screen (px), each a compensated pair. ScanPair<float2> on the C++ side, i.e. the two high
// parts first: .xy = hi (world, screen), .zw = lo (world, screen).
float2 WorldArc(float4 distances) { return distances.xz; }
float2 ScreenArc(float4 distances) { return distances.yw; }

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

static const uint CurveJoinRound  = 0u;
static const uint CurveJoinSquare = 1u;

// bezier_data_map entries (bezier_common.h), one per sample: the curve index in the low 30 bits, then
// whether the stroke goes on past the start and past the end of the segment starting on that sample.
static const uint SegmentCurveMask = 0x3FFFFFFFu;
static const uint SegmentHasPrev   = 0x40000000u;
static const uint SegmentHasNext   = 0x80000000u;

struct PatternStyle {
    uint  width_capcapjoin; // width << 24 | front_cap << 16 | back_cap << 8 | join
    float spacing;          // world arc length between dash centers; > 0
    float dash_length;      // length of one dash in pixels, not including caps
};

struct PatternedVSOutput {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;
    // rgb: color. w: the pattern coordinate - the position along the chain's center grid measured in
    // slots of PatternPosition, RELATIVE to PatternSlot below, so PatternSlot + floor() gives the slot.
    // Kept relative so it stays a small number with a precise fraction however long the chain is; it is
    // never below -1, and never below 0 on a chain's first curve (see curve_vs.hlsl). The chain's last
    // center is marked in PatternPosition itself (see curve_pattern_calc.hlsl).
    noperspective float4 ColorPattern : COLOR0;
    // The screen arc where this segment starts (after near-plane clipping), as a compensated pair: the
    // pixel shader only ever subtracts it from a dash center's arc, which is a pair too.
    nointerpolation float2 ScreenArcBegin : TEXCOORD1;
    // Whether the segment has a neighbor before its start (x) and after its end (y) - no neighbor = terminus.
    nointerpolation uint2 Neighbors : TEXCOORD2;
    nointerpolation float DashLength : TEXCOORD3;
    nointerpolation uint CapCapJoin : TEXCOORD4;
    // Signed tan(theta/2) of the screen-space turn at the start (x, SDF.y == 0) and the end
    // (y, SDF.y == SDF.w); 0 without a neighbor. See PatternArc in curve_ps.hlsl.
    nointerpolation float2 ArcShear : TEXCOORD5;
    // The PatternPosition slot of the curve's first grid point (PatternOffsets[curve]); ColorPattern.w
    // counts from here.
    nointerpolation uint PatternSlot : TEXCOORD6;
};

// Inverse of PackFloat3ToR8G8B8A8 (DirectX::XMCOLOR): a << 24 | r << 16 | g << 8 | b.
float4 UnpackColorBits(uint packed) {
    uint4 unpacked = uint4(packed, packed, packed, packed);
    unpacked >>= uint4(16, 8, 0, 24);
    unpacked &= 0xFF;
    return float4(unpacked) / 255.0;
}

// One row per coordinate, one column per control point: row x = (P0.x, P1.x, P2.x, P3.x), likewise y
// and z, so P3 sits in the .w of every row. row_major so the rows sit in memory in that order, matching
// the CPU's XMFLOAT3X4 whatever the compiler's default matrix packing is. 48 B, three 16-byte loads.
struct Cubic {
    row_major float3x4 P;
};

float3 EvaluateBezier(Cubic c, float t) {
    float s   = 1.0 - t;
    float st3 = 3.0 * s * t;
    return mul(c.P, float4(s * s * s, st3 * s, st3 * t, t * t * t));
}

float SampleT(uint2 range, uint sampleIndex) {
    return float(sampleIndex - range.x) / float(range.y - range.x); // Driver can cause problem if it compiles this to n * (1 / n)
}

// If `p` is behind the near plane (clip z < 0), slides it along the segment toward `other` onto the
// plane; otherwise returns it unchanged. `other` must be in front of the plane.
float4 ClipToNearPlane(float4 p, float4 other) {
    return (p.z < 0.0) ? lerp(p, other, p.z / (p.z - other.z)) : p;
}

float HalfWidth(uint style) { return float(style >> 24); }
uint FrontCap(uint capCapJoin) { return (capCapJoin >> 16) & 0xFF; }
uint BackCap(uint capCapJoin) { return (capCapJoin >> 8) & 0xFF; }
uint Join(uint capCapJoin) { return capCapJoin & 0xFF; }

#endif
