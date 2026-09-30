#ifndef LINE_COMMON_HLSLI
#define LINE_COMMON_HLSLI

// Per-curve data, split by how often it changes - see bezier_line.h. The control points are a plain
// StructuredBuffer<float3> holding each curve's Bezier control points in their native degree, packed
// back to back: 2 for a line, 3 for a quadratic, 4 for a cubic. Indices says where a curve's run is.
struct ColorData {
    uint4 c0_c1_height0_height1; // ColorBegin, ColorEnd, asuint(MinHeight), asuint(MaxHeight)
};

struct Indices {
    // FirstIndex, LastIndex: the curve's sample range, both inclusive.
    // .zw: its control points in control_points as [first, end) - END EXCLUSIVE, so .w - .z is the
    // point count (degree + 1), which is what Eval() loops over.
    uint4 first_last_first_bez_last_bez;
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
