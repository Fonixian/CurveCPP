#ifndef LINE_COMMON_HLSLI
#define LINE_COMMON_HLSLI

// Per-curve data, split by how often it changes - see bezier_line.h. The control points are a plain
// StructuredBuffer<float3>, four per curve: the monomial coefficients K0..K3, P(t) = K0 + t(K1 + t(K2 + tK3)).
struct ColorData {
    uint4 c0_c1_height0_height1; // ColorBegin, ColorEnd, asuint(MinHeight), asuint(MaxHeight)
};

struct Indices {
    uint2 first_last; // FirstIndex, LastIndex
};

// Straight from the vertex shader to the rasteriser: there is no geometry stage, so the line is the
// hardware's own 1px LINELIST primitive and this is also the pixel shader's input.
struct LineVSOutput {
    float4 Position : SV_Position;
    float3 Color : COLOR0;
};

float4 UnpackColorBits(uint packed) {
    return float4(
        float( packed        & 0xFF) / 255.0,
        float((packed >>  8) & 0xFF) / 255.0,
        float((packed >> 16) & 0xFF) / 255.0,
        float((packed >> 24) & 0xFF) / 255.0);
}

#endif
