#ifndef SOLID_COMMON_HLSLI
#define SOLID_COMMON_HLSLI

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

static const uint CurveJoinRound  = 0u;
static const uint CurveJoinSquare = 1u;

static const uint CurveEndJoined = 0xFFu;

struct SolidVSOutput {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    nointerpolation uint CapCapJoin : TEXCOORD1;
};

float4 UnpackColorBits(uint packed) {
    uint4 unpacked_u = uint4(packed, packed, packed, packed);
    unpacked_u >>= uint4(0, 8, 16, 24);
    unpacked_u &= 0xFF;
    return float4(unpacked_u) / 255.0;
}

float Width(uint width_capcapjoin) { return float(width_capcapjoin >> 24); }
uint FrontCap(uint capcapjoin) { return (capcapjoin >> 16) & 0xFF; }
uint BackCap(uint capcapjoin) { return (capcapjoin >> 8) & 0xFF; }
uint Join(uint capcapjoin) { return capcapjoin & 0xFF; }

#endif
