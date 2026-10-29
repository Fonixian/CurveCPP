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

// bezier_data_map entries (bezier_common.h), one per sample: the curve index in the low 30 bits, then
// whether the stroke goes on past the start and past the end of the segment starting on that sample.
static const uint SegmentCurveMask = 0x3FFFFFFFu;
static const uint SegmentHasPrev   = 0x40000000u;
static const uint SegmentHasNext   = 0x80000000u;

struct SolidVSOutput {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    nointerpolation uint CapCapJoin : TEXCOORD1;
};

// Inverse of PackFloat3ToR8G8B8A8 (DirectX::XMCOLOR): a << 24 | r << 16 | g << 8 | b.
float4 UnpackColorBits(uint packed) {
    uint4 unpacked = uint4(packed, packed, packed, packed);
    unpacked >>= uint4(16, 8, 0, 24);
    unpacked &= 0xFF;
    return float4(unpacked) / 255.0;
}

// style: half_width << 24 | front_cap << 16 | back_cap << 8 | join
float HalfWidth(uint style) { return float(style >> 24); }
uint FrontCap(uint capCapJoin) { return (capCapJoin >> 16) & 0xFF; }
uint BackCap(uint capCapJoin) { return (capCapJoin >> 8) & 0xFF; }
uint Join(uint capCapJoin) { return capCapJoin & 0xFF; }

#endif
