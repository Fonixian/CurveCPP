#ifndef CURVE_COMMON_HLSLI
#define CURVE_COMMON_HLSLI

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

static const uint CurveJoinRound  = 0u;
static const uint CurveJoinSquare = 1u;

struct PatternStyle {
    uint  width_capcapjoin; // width << 24 | front_cap << 16 | back_cap << 8 | join
    float spacing;          // world arc length between dash centers; > 0
    float dash_length;      // length of one dash in pixels, not including caps
};

struct PatternedVSOutput {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;
    // rgb: color. w: the pattern coordinate - the position along the chain's center grid measured
    // in slots of PatternPosition, so floor() gives the slot directly (see curve_vs.hlsl).
    noperspective float4 ColorPattern : COLOR0;
    nointerpolation float ScreenArcBegin : TEXCOORD1;
    // Whether the segment has a neighbor before its start (x) and after its end (y) - no neighbor = terminus.
    nointerpolation uint2 Neighbors : TEXCOORD2;
    nointerpolation float DashLength : TEXCOORD3;
    nointerpolation uint CapCapJoin : TEXCOORD4;
    // Signed tan(theta/2) of the screen-space turn at the start (x, SDF.y == 0) and the end
    // (y, SDF.y == SDF.w); 0 without a neighbor. See PatternArc in curve_ps.hlsl.
    nointerpolation float2 ArcShear : TEXCOORD5;
    // [x, y): the slots of PatternPosition owned by this segment's chain. PatternPosition is one
    // sorted run per chain, each starting again at screen arc 0, so any other slot is another
    // stroke's center. x == y: patterned, but no center at all (all gap).
    nointerpolation uint2 PatternSlots : TEXCOORD6;
};

float4 UnpackColorBits(uint packed) {
    uint4 unpacked = uint4(packed, packed, packed, packed);
    unpacked >>= uint4(0, 8, 16, 24);
    unpacked &= 0xFF;
    return float4(unpacked) / 255.0;
}

struct Cubic {
    float3 P0, P1, P2, P3;
};

float3 EvaluateBezier(Cubic c, float t) {
    float s   = 1.0 - t;
    float st3 = 3.0 * s * t;
    return mad(c.P3, t * t * t, mad(c.P2, st3 * t, mad(c.P1, st3 * s, c.P0 * (s * s * s))));
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
