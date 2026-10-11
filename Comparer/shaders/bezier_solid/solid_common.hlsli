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
    uint4 unpacked = uint4(packed, packed, packed, packed);
    unpacked >>= uint4(0, 8, 16, 24);
    unpacked &= 0xFF;
    return float4(unpacked) / 255.0;
}

// style: half_width << 24 | front_cap << 16 | back_cap << 8 | join
float HalfWidth(uint style) { return float(style >> 24); }
uint FrontCap(uint capCapJoin) { return (capCapJoin >> 16) & 0xFF; }
uint BackCap(uint capCapJoin) { return (capCapJoin >> 8) & 0xFF; }
uint Join(uint capCapJoin) { return capCapJoin & 0xFF; }

#endif
