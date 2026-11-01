#include "DotCommon.hlsli"

float CapSDF(float2 coord, float overshoot, float halfWidth, uint cap)
{
  switch (cap)
  {
  case CurveCapSquare: return overshoot - halfWidth;
  case CurveCapTriangleOut: return (coord.x + overshoot - halfWidth) * rsqrt(2.0);
  case CurveCapTriangleIn: return (overshoot - coord.x) * rsqrt(2.0);
  default: return overshoot; // butt
  }
}

// A dot is a zero-length dash: front cap on the half facing the curve's start, back cap on the other.
float DotSDF(float2 offset, float halfWidth, uint capCapJoin)
{
  float2 coord = float2(abs(offset.x), offset.y);
  uint cap = (offset.y < 0.0) ? FrontCap(capCapJoin) : BackCap(capCapJoin);

  if (cap == CurveCapRound) return length(coord) - halfWidth;
  return max(coord.x - halfWidth, CapSDF(coord, abs(offset.y), halfWidth, cap));
}

float4 main(DotVSOutput input) : SV_Target
{
  float sdf = DotSDF(input.Offset, HalfWidth(input.CapCapJoin), input.CapCapJoin);
  if (sdf >= DOT_AA_RADIUS) discard;

  return float4(input.Color.rgb, input.Color.a * (1.0 - smoothstep(-DOT_AA_RADIUS, DOT_AA_RADIUS, sdf)));
}
