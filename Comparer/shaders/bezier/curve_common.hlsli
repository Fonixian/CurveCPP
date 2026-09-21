#ifndef CURVE_COMMON_HLSLI
#define CURVE_COMMON_HLSLI

// K0..K3 are the curve in the MONOMIAL basis, NOT its control points:
//     P(t) = K0 + t * (K1 + t * (K2 + t * K3))
// The CPU converts once per upload (ToPowerBasis in bezier_common.cpp); the slots and the 80-byte
// layout are unchanged, so this costs nothing to load. Do not read K1 expecting P1.
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
    float2 Padding;
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

struct CurveVSOutput {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    noperspective float  TotalDistance : TEXCOORD1;
    nointerpolation float Spacing : TEXCOORD2;
    nointerpolation float ScreenArcBegin : TEXCOORD3;
    nointerpolation float ScreenArcEnd : TEXCOORD8;
    nointerpolation float DashLength : TEXCOORD4;
    // How a CHAIN-global centre index turns into a slot in the flat pattern array:
    //     slot = clamp(globalIndex + PatternSlot.x, PatternSlot.y, PatternSlot.z)
    // x is first slot - first global index, so it undoes the window curve_pattern_ini gave this
    // curve. y/z are the slots this curve may read: its own slice widened by one centre at each end
    // where a neighbour has one, because a dash centred just across a joint still has to reach back
    // over it. Both ends stay inside the region pattern_calc wrote, and z < y says the chain holds
    // no centre at all - the curve is then all gap, NOT solid. Solid is Spacing <= 0, nothing else.
    //
    // This replaced a uint2 (first, count) range: a curve that owns no centre of its own is a
    // normal thing now, so its count no longer answers any question the pixel shader asks.
    nointerpolation int3 PatternSlot : TEXCOORD5;
    nointerpolation uint CapCapJoin : TEXCOORD6;
    // Signed tan(theta/2) of the screen-space turn at each joint: x at B (SDF.y == 0), y at C
    // (SDF.y == l_CB). 0 where there is no neighbour. Turns the segment-local arc into the arc
    // measured against the joint's angle bisector - see CurvePatternArc in curve_ps.hlsl.
    nointerpolation float2 ArcShear : TEXCOORD7;
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

#endif
