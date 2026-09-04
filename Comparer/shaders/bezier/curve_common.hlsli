#ifndef CURVE_COMMON_HLSLI
#define CURVE_COMMON_HLSLI

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

struct CurveStyle {
    float Width;
    uint  CapCapJoinPattern;
    float Spacing;
    float DashLength;
};

// Must match curve::CurveCap / curve::CurveJoin / curve::CurvePattern in bezier.h.
static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

static const uint CurveJoinRound  = 0u;
static const uint CurveJoinSquare = 1u;

static const uint CurvePatternSolid = 0u;
static const uint CurvePatternDash  = 1u;
static const uint CurvePatternDot   = 2u;

struct CurveVSOutput {
    float4 Position : SV_Position;
    // x: lateral distance from the centre line (px, signed)
    // y: longitudinal distance from point B ALONG the segment (px) - SEGMENT-LOCAL, 0 at B and
    //    l_CB at C. The pixel shader needs it segment-local for the join/cap rounding, and adds
    //    ScreenArcBegin to it to recover the curve-global screen arc length.
    // z: half-width in px   w: l_CB (segment length in px)
    noperspective float4 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    noperspective float  TotalDistance : TEXCOORD1;  // Cumulative WORLD arc length
    nointerpolation float Spacing : TEXCOORD2;       // World-space pattern spacing
    // Screen arc length at point B. Constant across the segment, so SDF.y + this is exact rather
    // than an interpolation of two per-vertex cumulative values.
    nointerpolation float ScreenArcBegin : TEXCOORD3;
    // Screen arc length at which the CURVE ends, or +inf when this segment is followed by another
    // one. A pixel outside [0, ScreenArcEnd] has no neighbouring segment, so it gets the cap
    // treatment instead of the join treatment.
    nointerpolation float ScreenArcEnd : TEXCOORD8;
    nointerpolation float DashLength : TEXCOORD4;    // Half-length of one dash/dot, in px
    nointerpolation uint2 PatternRange : TEXCOORD5;  // x = first pattern index, y = count
    nointerpolation uint CapCapJoinPattern : TEXCOORD6;
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

uint FrontCap(uint capcapjoin)
{
    return (capcapjoin >> 24) & 0xFF;
}
uint BackCap(uint capcapjoin)
{
    return (capcapjoin >> 16) & 0xFF;
}
uint Join(uint capcapjoin)
{
    return (capcapjoin >> 8) & 0xFF;
}
uint Pattern(uint capcapjoin)
{
    return capcapjoin & 0xFF;
}

#endif
