#ifndef CURVE_COMMON_HLSLI
#define CURVE_COMMON_HLSLI

struct BezierCurveData {
    float3 K0;
    int    FirstIndex;
    float3 K1;
    int    LastIndex;
    float3 K2;
    uint   ColorBegin;
    float3 K3;
    uint   ColorEnd;
    float  MinHeight;
    float  MaxHeight;
    // First / last sample of the whole merged chain (== FirstIndex / LastIndex when unmerged).
    int    ChainFirstIndex;
    int    ChainLastIndex;
};

struct CurveStyle {
    float Width;
    uint  CapCapJoin;
    float Spacing;
    float DashLength;
};

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

static const uint CurveJoinRound  = 0u;
static const uint CurveJoinSquare = 1u;

// Bit 24 of CapCapJoin, set by the vertex shader (the CPU packs only the three cap/join bytes).
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
    nointerpolation float ScreenArcEnd : TEXCOORD2;
    nointerpolation float DashLength : TEXCOORD3;
    nointerpolation uint CapCapJoin : TEXCOORD4;
    // Signed tan(theta/2) of the screen-space turn at each joint: x at B (SDF.y == 0), y at C
    // (SDF.y == l_CB). 0 where there is no neighbour. Turns the segment-local arc into the arc
    // measured against the joint's angle bisector - see CurvePatternArc in curve_ps.hlsl.
    nointerpolation float2 ArcShear : TEXCOORD5;
};

float4 UnpackColorBits(uint packed) {
    return float4(
        float( packed        & 0xFF) / 255.0,
        float((packed >>  8) & 0xFF) / 255.0,
        float((packed >> 16) & 0xFF) / 255.0,
        float((packed >> 24) & 0xFF) / 255.0);
}

uint PackColorBits(float4 color) {
    uint r = uint(saturate(color.r) * 255.0 + 0.5);
    uint g = uint(saturate(color.g) * 255.0 + 0.5);
    uint b = uint(saturate(color.b) * 255.0 + 0.5);
    uint a = uint(saturate(color.a) * 255.0 + 0.5);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

uint FrontCap(uint capcapjoin) { return (capcapjoin >> 16) & 0xFF; }
uint BackCap(uint capcapjoin) { return (capcapjoin >> 8) & 0xFF; }
uint Join(uint capcapjoin) { return capcapjoin & 0xFF; }
bool IsPatterned(uint capcapjoin) { return (capcapjoin & CurvePatternedBit) != 0u; }

#endif