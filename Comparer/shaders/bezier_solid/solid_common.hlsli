#ifndef SOLID_COMMON_HLSLI
#define SOLID_COMMON_HLSLI

// How much of the pixel shader's coverage ramp the GEOMETRY is allowed to let through, in pixels.
// solid_ps.hlsl fades over sdf in [-0.5, +0.5], so 0.5 is the value that draws the whole ramp and
// makes the stroke come out exactly halfWidth wide. The old 5-vertex strip ended at |lateral| ==
// halfWidth, i.e. it clipped the outer half of the ramp and drew every stroke half a pixel thin with
// a hard edge - set this to 0.0 to get that look back. FxCompile-overridable.
#ifndef SOLID_AA_MARGIN
#define SOLID_AA_MARGIN 0.5
#endif

// Miter limit for CurveJoinSquare, in half-widths: how far past a joint a square join may reach
// before it is cut off flat. 1.0 reproduces the old geometry exactly for every turn up to 90 deg
// (a plain miter is at most one half-width long there) and bevels the sharper ones, which is what
// the old join wedge did too. Raising it lengthens the spike at sharp turns AND grows the covering
// triangle at those joints. Round joins ignore it - a disc never reaches past halfWidth.
#ifndef SOLID_MITER_LIMIT
#define SOLID_MITER_LIMIT 1.0
#endif

// Slack added to the covering triangle ALONG THE SEGMENT so its edges cannot graze the exact corner
// of the region the pixel shader wants to keep. Pure insurance, invisible: the pixel shader discards
// there anyway. Not applied laterally, so the stroke's own edge stays where SOLID_AA_MARGIN puts it.
#ifndef SOLID_TRIANGLE_PAD
#define SOLID_TRIANGLE_PAD 0.25
#endif

static const float SolidAAMargin    = SOLID_AA_MARGIN;
static const float SolidMiterLimit  = SOLID_MITER_LIMIT;
static const float SolidTrianglePad = SOLID_TRIANGLE_PAD;

struct BezierCurveData {
    float3 P0;
    int    FirstIndex;
    float3 P1;
    int    LastIndex;
    float3 P2;
    uint   ColorBegin;
    float3 P3;
    uint   ColorEnd;
    float  MinHeight;
    float  MaxHeight;
    float2 Padding;
};

struct SolidCurveStyle {
    float Width;
    uint CapCapJoin;
    float Padding[2];
};

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

static const uint CurveJoinRound  = 0u;
static const uint CurveJoinSquare = 1u;

// What sits at one end of a segment. The 3-vertex geometry is a loose covering triangle, so the
// pixel shader - not the rasteriser - decides where the segment stops; this says how.
static const uint SolidTerminus      = 0u; // no neighbour: the cap SDF alone bounds this end
static const uint SolidJointBisector = 1u; // a real joint: the angle bisector splits it with the neighbour
static const uint SolidJointFlat     = 2u; // folded joint, or an end the near plane cut: flat truncation

struct SolidVSOutput {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    nointerpolation uint CapCapJoin : TEXCOORD1;
    nointerpolation uint2 Neighbors : TEXCOORD2;
    // Signed tan(half the turn) at each end, x = joint at B, y = joint at C, in the lateral frame
    // below. The joint's angle bisector is the line localArc + lateral * ArcShear == the joint's own
    // arc, and both segments meeting there compute the same line, so it splits the joint between
    // them with no gap and no double blend. 0 at a terminus or a fold.
    nointerpolation float2 ArcShear : TEXCOORD3;
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

// How far past a joint the pixel shader may draw, in pixels. A round join is a disc of radius
// halfWidth around the joint (plus the coverage ramp); a square join is a miter, limited.
float SolidJoinReach(uint capcapjoin, float halfWidth) {
    return (Join(capcapjoin) == CurveJoinRound) ? (halfWidth + SolidAAMargin)
                                                : (halfWidth * SolidMiterLimit);
}

// How far past a TERMINUS a cap may draw. Butt needs the ramp only, Square and Round reach
// halfWidth + ramp, TriangleOut reaches halfWidth + ramp*sqrt2, and TriangleIn - the greediest -
// reaches |lateral| + ramp*sqrt2 <= halfWidth + ramp*(1 + sqrt2). That last one is what the
// covering triangle is sized by, so no cap can fall outside it.
float SolidCapReach(float halfWidth) {
    return halfWidth + SolidAAMargin * 2.4142136;
}

#endif
