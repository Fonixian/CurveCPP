#ifndef SOLID_COMMON_HLSLI
#define SOLID_COMMON_HLSLI

// One per curve, 32 bytes - UploadSolidCurveData in bezier_solid.h, which documents every field.
// The curve's control points are NOT in here: they sit in their own float3 buffer (ControlPoints in
// solid_vert.hlsl), 2, 3 or 4 of them as given, starting at ControlFirst.
struct BezierCurveData {
    uint  ControlFirst;   // first control point in ControlPoints
    uint  FirstIndex;     // first sample index of this curve
    uint  Resolution;     // sample count, both ends included
    uint  CountCapCapJoin; // control point count << 24 | front cap << 16 | back cap << 8 | join
    uint  ColorBegin;     // R8G8B8A8
    uint  ColorEnd;       // R8G8B8A8
    uint  HeightRange;    // two halves: min height low 16 bits, max height high 16 bits
    float Width;          // px
};

// [first, one past last) control point of a curve - 2, 3 or 4 points, degree 1, 2 or 3.
uint2 ControlRange(BezierCurveData bez) {
    return uint2(bez.ControlFirst, bez.ControlFirst + (bez.CountCapCapJoin >> 24));
}
float MinHeight(BezierCurveData bez) { return f16tof32(bez.HeightRange); }
float MaxHeight(BezierCurveData bez) { return f16tof32(bez.HeightRange >> 16); }

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

static const uint CurveJoinRound  = 0u;
static const uint CurveJoinSquare = 1u;

struct SolidVSOutput {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    nointerpolation uint CapCapJoin : TEXCOORD1;
    nointerpolation uint2 Neighbors : TEXCOORD2;
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
