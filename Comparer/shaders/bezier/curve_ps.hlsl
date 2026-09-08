#include "curve_common.hlsli"

#ifndef CURVE_PATTERN_SHRINK_TO_FIT
#define CURVE_PATTERN_SHRINK_TO_FIT 1
#endif

#ifndef CURVE_PATTERN_FILL
#define CURVE_PATTERN_FILL 0.95
#endif

StructuredBuffer<float> PatternPosition : register(t1);

static const float CurveInvSqrt2 = 0.70710678118;
static const float CurveSdfInside = -1e30;

// --- shared building blocks --------------------------------------------------------
float CurveBodySDF(float2 coord, float halfWidth)
{
    return coord.x - halfWidth;
}
float CurveEndOvershoot(float2 coord, float spanLength)
{
    return max(-coord.y, coord.y - spanLength);
}

float CurveRoundEndSDF(float2 coord, float spanLength, float halfWidth)
{
    const float overshoot = CurveEndOvershoot(coord, spanLength);
    return (overshoot > 0.0)
        ? length(float2(coord.x, overshoot)) - halfWidth
        : CurveBodySDF(coord, halfWidth);
}

// --- end caps ----------------------------------------------------------------------
float CurveCapSDF(float2 coord, float spanLength, float halfWidth, uint cap)
{
    const float body      = CurveBodySDF(coord, halfWidth);
    const float overshoot = CurveEndOvershoot(coord, spanLength);

    if (cap == CurveCapButt)
    {
        return max(body, overshoot);
    }
    if (cap == CurveCapSquare)
    {
        return max(body, overshoot - halfWidth);
    }
    if (cap == CurveCapTriangleOut)
    {
        return max(body, (coord.x + overshoot - halfWidth) * CurveInvSqrt2);
    }
    if (cap == CurveCapTriangleIn)
    {
        return max(body, (overshoot - coord.x) * CurveInvSqrt2);
    }
    // CurveCapRound
    return CurveRoundEndSDF(coord, spanLength, halfWidth);
}

// --- joins -------------------------------------------------------------------------
float CurveJoinSDF(float2 coord, float spanLength, float halfWidth, uint join)
{
    if (join == CurveJoinRound)
    {
        return CurveRoundEndSDF(coord, spanLength, halfWidth);
    }
    return CurveBodySDF(coord, halfWidth);
}

float CurveStrokeSDF(
    float2 coord,
    float  segmentLength,
    float  halfWidth,
    bool   atTerminus,
    uint   cap,
    uint   join)
{
    if (!atTerminus)
    {
        return CurveJoinSDF(coord, segmentLength, halfWidth, join);
    }
    return CurveCapSDF(coord, segmentLength, halfWidth, cap);
}

// --- dash pattern ------------------------------------------------------------------

#if CURVE_PATTERN_SHRINK_TO_FIT
float CurvePatternCentreSpan(uint2 patternRange, int i0, int i1, float c0, float c1)
{
    if (i1 != i0)
    {
        return abs(c1 - c0);
    }
    if (i0 > 0)
    {
        return abs(c0 - PatternPosition[patternRange.x + uint(i0 - 1)]);
    }
    return 0.0;
}

float CurvePatternElementLength(float dashLength, float halfWidth, uint cap)
{
    return dashLength + ((cap == CurveCapButt) ? 0.0 : 2.0 * halfWidth);
}

float CurvePatternArcScale(float centreSpan, float elementLength)
{
    const float budget = centreSpan * CURVE_PATTERN_FILL;
    return (budget > 1e-6) ? max(1.0, elementLength / budget) : 1.0;
}

#endif

float CurvePatternSDF(
    float  lateral,
    float  currentArc,
    float  totalDistance,
    float  spacing,
    uint2  patternRange,
    float  dashLength,
    float  halfWidth,
    uint   capCapJoin)
{
    if (patternRange.y == 0u)
    {
        return CurveSdfInside;
    }
    
    const int lastPattern = int(patternRange.y) - 1;
    const int i0 = clamp(int(floor(totalDistance / max(spacing, 1e-6))), 0, lastPattern);
    const int i1 = min(i0 + 1, lastPattern);

    const float c0 = PatternPosition[patternRange.x + uint(i0)];
    const float c1 = PatternPosition[patternRange.x + uint(i1)];

    const float a0 = currentArc - c0;
    const float a1 = currentArc - c1;
    float arcDist = (abs(a0) <= abs(a1)) ? a0 : a1;
    uint cap = totalDistance > arcDist ? FrontCap(capCapJoin) : BackCap(capCapJoin);
    
#if CURVE_PATTERN_SHRINK_TO_FIT
    arcDist *= CurvePatternArcScale(
        CurvePatternCentreSpan(patternRange, i0, i1, c0, c1),
        CurvePatternElementLength(dashLength, halfWidth, cap));
#endif
    return CurveCapSDF(float2(abs(lateral), arcDist + dashLength * 0.5), dashLength, halfWidth, cap);
}

// -----------------------------------------------------------------------------------

float4 main(CurveVSOutput input) : SV_Target
{
    const float lateral       = input.SDF.x;
    const float localArc      = input.SDF.y;
    const float halfWidth     = input.SDF.z;
    const float segmentLength = input.SDF.w;

    const float2 coord = float2(abs(lateral), localArc);
    
    const float currentArc = input.ScreenArcBegin + localArc;
    
    const bool atTerminus = (currentArc < 0.0) || (currentArc > input.ScreenArcEnd);

    const uint cap = (currentArc < 0.0) ? FrontCap(input.CapCapJoin) : BackCap(input.CapCapJoin);

    float sdf = CurveStrokeSDF(
        coord,
        segmentLength,
        halfWidth,
        atTerminus,
        cap,
        Join(input.CapCapJoin));

    sdf = max(sdf, CurvePatternSDF(
        lateral,
        currentArc,
        input.TotalDistance,
        input.Spacing,
        input.PatternRange,
        input.DashLength,
        halfWidth,
        input.CapCapJoin));

    if (sdf > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - sdf));
}
