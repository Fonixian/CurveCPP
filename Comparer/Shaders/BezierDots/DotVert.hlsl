#include "DotCommon.hlsli"

// Drawn as DrawInstancedIndirect(4, dot count) with a TRIANGLESTRIP, one quad per dot:
//
//                 2 ------- 3
//                 |    ^    |
//                 |    *    |    * : the dot's center, the curve point at the dot's t
//                 |         |    ^ : the screen tangent, towards the curve's end
//                 0 ------- 1
//
// The quad reaches QuadExtent (the half-width plus the antialiasing radius) from the center, so the
// antialiased edge fits. The pixel shader gets the position in pixels from the center.
// Everything comes from the dot's curve parameter t: the position is the curve evaluated there, the
// direction is the curve's derivative there projected to the screen. A degenerate derivative (a cusp,
// or a curve seen end-on) falls back to the screen's x axis.

cbuffer CameraData : register(b1)
{
  float4x4 VP;
  float2 WH;
  uint TotalPointCount;
  uint TotalCurveCount;
};

StructuredBuffer<Cubic> CurveControlPoints : register(t3); // one 3x4 per curve, rows x/y/z, columns P0..P3
StructuredBuffer<DotSample> Dots : register(t4);           // where SolidVert has the sample map
StructuredBuffer<uint4> CurveColors : register(t5);
StructuredBuffer<DotStyle> CurveStyles : register(t6);

Cubic LoadCubic(uint curveIndex)
{
  return CurveControlPoints[curveIndex];
}

float4 CalculateColor(uint4 colorData, float height, float t)
{
  float minHeight = asfloat(colorData.z);
  float maxHeight = asfloat(colorData.w);
  float3 colorA = UnpackColorBits(colorData.x).rgb;
  float3 colorB = UnpackColorBits(colorData.y).rgb;
  float blend = (minHeight < maxHeight) ? saturate((height - minHeight) / (maxHeight - minHeight)) : t;
  return float4(lerp(colorA, colorB, blend), 1.0);
}

DotVSOutput main(uint vertexId : SV_VertexID, uint dotIndex : SV_InstanceID)
{
  DotVSOutput o = (DotVSOutput)0;

  DotSample dotSample = Dots[dotIndex];
  uint curveIndex = dotSample.CurveIndex;
  float t = dotSample.T;

  Cubic cubic = LoadCubic(curveIndex);

  float3 world = EvaluateBezier(cubic, t);
  float4 clip = mul(float4(world, 1.0), VP);

  // Behind the camera: drop the dot. Unlike a strip there is no continuity to keep, so no clipping.
  if (clip.w <= 0.0)
  {
    o.Position = 0.0 / 0.0;
    return o;
  }

  float4 dClip = mul(float4(EvaluateBezierDerivative(cubic, t), 0.0), VP);
  float2 dir = (dClip.xy * clip.w - clip.xy * dClip.w) / (clip.w * clip.w) * (0.5 * WH);
  float dirLength = length(dir);

  dir = (dirLength > 1e-5) ? (dir / dirLength) : float2(1.0, 0.0);
  float2 right = float2(dir.y, -dir.x);

  uint style = CurveStyles[curveIndex].width_capcap;
  float extent = HalfWidth(style) + DOT_AA_RADIUS;

  o.Color = CalculateColor(CurveColors[curveIndex], world.y, t);
  o.CapCapJoin = style; // the pixel shader reads the half-width from the width byte; FrontCap/BackCap mask it out

  float2 corner = float2((vertexId & 1u) ? 1.0 : -1.0, (vertexId & 2u) ? 1.0 : -1.0);
  o.Offset = corner * extent;

  o.Position = float4(clip.xyz / clip.w, 1.0);
  o.Position.xy = mad(2.0 / WH, extent * (corner.x * right + corner.y * dir), o.Position.xy);

  return o;
}
