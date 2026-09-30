#ifndef SOLID_COMMON_HLSLI
#define SOLID_COMMON_HLSLI

// Per-curve data, split by how often it changes - see BezierSplitRendererBase in bezier_common.h.
// The control points are a plain StructuredBuffer<float3>, four per curve at curveIndex * 4: the cubic
// in monomial form K0..K3, P(t) = K0 + t(K1 + t(K2 + tK3)) - the same coefficients UploadBezierData
// carries. Same layout as line_common.hlsli, duplicated on purpose.
struct ColorData {
    uint4 c0_c1_height0_height1; // ColorBegin, ColorEnd, asuint(MinHeight), asuint(MaxHeight)
};

struct Indices {
    uint2 first_last; // FirstIndex, LastIndex: the curve's sample range, both inclusive
};

struct SolidCurveStyle {
    // width << 24 | CapCapJoin: width in whole pixels [0, 255] in the top byte, then the usual
    // front << 16 | back << 8 | join - so FrontCap/BackCap/Join below read it as-is.
    uint width_capcapjoin;
};

float StyleWidth(uint width_capcapjoin) { return float(width_capcapjoin >> 24); }

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
    uint4 unpacked_u = uint4(packed, packed, packed, packed);
    unpacked_u >>= uint4(0, 8, 16, 24);
    unpacked_u &= 0xFF;
    return float4(unpacked_u) / 255.0;
}

uint FrontCap(uint capcapjoin) { return (capcapjoin >> 16) & 0xFF; }
uint BackCap(uint capcapjoin) { return (capcapjoin >> 8) & 0xFF; }
uint Join(uint capcapjoin) { return capcapjoin & 0xFF; }

#endif
