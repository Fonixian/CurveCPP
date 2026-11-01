#include "CurveCommon.hlsli"

// Squeeze dashes so they never reach into the neighbouring one as the curve recedes.
#ifndef CURVE_PATTERN_SHRINK_TO_FIT
#define CURVE_PATTERN_SHRINK_TO_FIT 1
#endif

// Clamp the pattern arc to the segment's own arc range at joints, so the quad's overshoot past a
// joint does not read the pattern at arcs this segment never covers.
#ifndef CURVE_PATTERN_CLAMP_TO_SEGMENT
#define CURVE_PATTERN_CLAMP_TO_SEGMENT 1
#endif

// Share of the center-to-center span one dash may occupy.
#ifndef CURVE_PATTERN_FILL
#define CURVE_PATTERN_FILL 0.95
#endif

// Measure the arc against the joint's angle bisector near each end, so dashes don't kink at joints.
#ifndef CURVE_PATTERN_BISECTOR
#define CURVE_PATTERN_BISECTOR 1
#endif

// Screen arc of every dash center as a pair, one sorted run per chain; the sign bit of hi flags a
// chain's last center.
StructuredBuffer<float2> PatternPosition : register(t1);

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

// 1. Curve shape

float SegmentSDF(float2 coord, float segmentLength, float halfWidth, uint2 neighbors, uint capCapJoin)
{
  bool2 pastEnd = bool2(coord.y < 0.0, coord.y > segmentLength);
  uint endCap = pastEnd.x ? FrontCap(capCapJoin) : BackCap(capCapJoin);
  bool atEndCap = any(pastEnd && (neighbors == 0u));

  if (!atEndCap && Join(capCapJoin) != CurveJoinRound) return coord.x - halfWidth;
  return SpanSDF(coord, segmentLength, halfWidth, atEndCap ? endCap : CurveCapRound);
}

// 2. Pattern coordinates

#if CURVE_PATTERN_BISECTOR
// Shears the segment arc by lateral * tan(theta/2) inside a band of halfWidth * tan(theta/2) at each
// end; zero on the center line. Short segments shrink the bands, keeping a fifth of the segment as ramp.
float BisectorArc(float localArc, float lateral, float segmentLength, float halfWidth, float2 arcShear)
{
  float2 band = halfWidth * abs(arcShear);
  float want = band.x + band.y;
  float allow = segmentLength * 0.8;
  band *= (want > allow) ? (allow / max(want, 1e-6)) : 1.0;

  float ramp = max(segmentLength - band.x - band.y, 1e-3);
  float w = saturate((localArc - band.x) / ramp);
  return localArc + lateral * lerp(arcShear.x, arcShear.y, w);
}
#endif

// Screen arc from the segment start (ScreenArcBegin).
float PatternArc(PatternedVSOutput input)
{
  float lateral = input.SDF.x;
  float localArc = input.SDF.y;
  float halfWidth = input.SDF.z;
  float segmentLength = input.SDF.w;

#if CURVE_PATTERN_BISECTOR
  float arc = BisectorArc(localArc, lateral, segmentLength, halfWidth, input.ArcShear);
#else
  float arc = localArc;
#endif

#if CURVE_PATTERN_CLAMP_TO_SEGMENT
  float arcLo = (input.Neighbors.x != 0u) ? 0.0 : -1e30;
  float arcHi = (input.Neighbors.y != 0u) ? segmentLength : 1e30;
  arc = clamp(arc, arcLo, arcHi);
#endif
  return arc;
}

// 3. Pattern

bool IsChainLastCenter(float2 stored)
{
  return (asuint(stored.x) & 0x80000000u) != 0u;
}

float2 CenterArc(float2 stored)
{
  return float2(asfloat(asuint(stored.x) & 0x7FFFFFFFu), stored.y);
}

#if CURVE_PATTERN_SHRINK_TO_FIT
// Distance between the two centers around the pixel; at a chain's last center, to the one before it.
float PatternCenterSpan(int slot0, bool isLast0, float2 center0, float2 center1)
{
  if (!isLast0) return abs(PairDiff(center1, center0));
  if (slot0 > 0)
  {
    float2 previous = PatternPosition[uint(slot0 - 1)];
    if (!IsChainLastCenter(previous)) return abs(PairDiff(center0, previous));
  }
  return 0.0;
}

float PatternArcScale(float centerSpan, float dashLength, float halfWidth, uint cap)
{
  float elementLength = dashLength + ((cap == CurveCapButt) ? 0.0 : 2.0 * halfWidth);
  float budget = centerSpan * CURVE_PATTERN_FILL;
  return (budget > 1e-6) ? max(1.0, elementLength / budget) : 1.0;
}
#endif

// segmentArc: the pixel's screen arc from arcBegin, the segment start.
float PatternSDF(float lateral, float segmentArc, float2 arcBegin, uint patternSlot, float patternCoord, float dashLength, float halfWidth, uint capCapJoin)
{
  int slot0 = max(int(patternSlot) + int(floor(patternCoord)), 0);

  float2 stored0 = PatternPosition[uint(slot0)];
  bool isLast0 = IsChainLastCenter(stored0);
  float2 center0 = CenterArc(stored0);
  float2 center1 = isLast0 ? center0 : CenterArc(PatternPosition[uint(slot0 + 1)]);

  float arc0 = segmentArc - PairDiff(center0, arcBegin);
  float arc1 = segmentArc - PairDiff(center1, arcBegin);
  float arcDist = (abs(arc0) <= abs(arc1)) ? arc0 : arc1;
  uint cap = (arcDist < 0.0) ? FrontCap(capCapJoin) : BackCap(capCapJoin);

#if CURVE_PATTERN_SHRINK_TO_FIT
  arcDist *= PatternArcScale(PatternCenterSpan(slot0, isLast0, center0, center1), dashLength, halfWidth, cap);
#endif

  return SpanSDF(float2(abs(lateral), arcDist + dashLength * 0.5), dashLength, halfWidth, cap);
}

float4 main(PatternedVSOutput input) : SV_Target
{
  float lateral = input.SDF.x;
  float halfWidth = input.SDF.z;

  float sdf = SegmentSDF(float2(abs(lateral), input.SDF.y), input.SDF.w, halfWidth, input.Neighbors, input.CapCapJoin);
  if (sdf > 0.5) discard;

  float segmentArc = PatternArc(input);

  sdf = max(sdf, PatternSDF(lateral, segmentArc, input.ScreenArcBegin, input.PatternSlot, input.PatternCoord, input.DashLength, halfWidth, input.CapCapJoin));
  if (sdf > 0.5) discard;

  return float4(input.Color.rgb, input.Color.a * saturate(0.5 - sdf));
}
