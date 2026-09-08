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
    nointerpolation uint2 PatternRange : TEXCOORD5;
    nointerpolation uint CapCapJoin : TEXCOORD6;
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
