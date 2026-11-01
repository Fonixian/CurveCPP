#include "SolidCommon.hlsli"

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

// Distance to a stroke running from y = 0 to y = spanLength, |x| <= halfWidth, closed by `cap` at both ends.
float SpanSDF(float2 coord, float spanLength, float halfWidth, uint cap)
{
  float overshoot = max(-coord.y, coord.y - spanLength);
  if (cap == CurveCapRound)
  {
    return (overshoot > 0.0) ? length(float2(coord.x, overshoot)) - halfWidth : coord.x - halfWidth;
  }
  return max(coord.x - halfWidth, CapSDF(coord, overshoot, halfWidth, cap));
}

float SegmentSDF(float2 coord, float segmentLength, float halfWidth, uint capCapJoin)
{
  bool2 pastEnd = bool2(coord.y < 0.0, coord.y > segmentLength);
  uint endCap = pastEnd.x ? FrontCap(capCapJoin) : BackCap(capCapJoin);
  bool atEndCap = any(pastEnd) && endCap != CurveEndJoined;

  if (!atEndCap && Join(capCapJoin) != CurveJoinRound) return coord.x - halfWidth;
  return SpanSDF(coord, segmentLength, halfWidth, atEndCap ? endCap : CurveCapRound);
}

float4 main(SolidVSOutput input) : SV_Target
{
  float sdf = SegmentSDF(float2(abs(input.SDF.x), input.SDF.y), input.SDF.w, input.SDF.z, input.CapCapJoin);
  if (sdf > 0.5) discard;

  return float4(input.Color.rgb, input.Color.a * saturate(0.5 - sdf));
}
