#ifndef LINE_COMMON_HLSLI
#define LINE_COMMON_HLSLI

// Per-curve data, split by how often it changes - see BezierSplitRendererBase in bezier_common.h.
// The control points are a plain StructuredBuffer<float3>, four per curve at curveIndex * 4: the cubic
// as its four Bezier control points P0..P3 (lower degrees raised to cubic on upload) - the same points UploadBezierData
// carries. Same layout as solid_common.hlsli, duplicated on purpose.
struct ColorData {
    uint4 c0_c1_height0_height1; // ColorBegin, ColorEnd, asuint(MinHeight), asuint(MaxHeight)
};

struct Indices {
    uint2 first_last; // FirstIndex, LastIndex: the curve's sample range, both inclusive
};

// segment_map entries (bezier_common.h): the curve index in the low 30 bits, then whether the stroke
// goes on past the segment's start and past its end - the line renderer only needs the curve.
static const uint SegmentCurveMask = 0x3FFFFFFFu;
static const uint SegmentHasPrev   = 0x40000000u;
static const uint SegmentHasNext   = 0x80000000u;

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
