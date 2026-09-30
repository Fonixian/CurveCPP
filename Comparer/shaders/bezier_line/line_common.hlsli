#ifndef LINE_COMMON_HLSLI
#define LINE_COMMON_HLSLI

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
    int    ChainFirstIndex;
    int    ChainLastIndex;
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
