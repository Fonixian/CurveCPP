#ifndef LINE_COMMON_HLSLI
#define LINE_COMMON_HLSLI

// Per-curve data, split by how often it changes - see BezierSplitRendererBase in BezierRendererBase.h.
// The control points are one row_major float3x4 per curve (see Cubic in LineVert.hlsl): the cubic
// as its four Bezier control points P0..P3, one per column, rows x/y/z (lower degrees raised to cubic
// on the way in). Same layout as SolidCommon.hlsli, duplicated on purpose.
struct ColorData
{
  uint4 c0_c1_height0_height1; // ColorBegin, ColorEnd, asuint(MinHeight), asuint(MaxHeight)
};

struct Indices
{
  uint2 first_last; // FirstIndex, LastIndex: the curve's sample range, both inclusive
};

// bezier_data_map entries (BezierRendererBase.cpp), one per sample: the curve index in the low 30 bits,
// then whether the stroke goes on past the start and past the end of the segment starting on that
// sample - the line renderer only needs the curve.
static const uint SegmentCurveMask = 0x3FFFFFFFu;
static const uint SegmentHasPrev = 0x40000000u;
static const uint SegmentHasNext = 0x80000000u;

// Straight from the vertex shader to the rasteriser: there is no geometry stage, so the line is the
// hardware's own 1px LINELIST primitive and this is also the pixel shader's input.
struct LineVSOutput
{
  float4 Position : SV_Position;
  float3 Color : COLOR0;
};

// Inverse of PackFloat3ToR8G8B8A8 (DirectX::XMCOLOR): a << 24 | r << 16 | g << 8 | b.
float4 UnpackColorBits(uint packed)
{
  return float4(
    float((packed >> 16) & 0xFF) / 255.0,
    float((packed >> 8) & 0xFF) / 255.0,
    float(packed & 0xFF) / 255.0,
    float((packed >> 24) & 0xFF) / 255.0);
}

#endif
