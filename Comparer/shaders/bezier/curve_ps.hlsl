#include "curve_common.hlsli"

#ifndef CURVE_PATTERN_SHRINK_TO_FIT
#define CURVE_PATTERN_SHRINK_TO_FIT 1
#endif

#ifndef CURVE_PATTERN_FILL
#define CURVE_PATTERN_FILL 0.95
#endif

// The arc a segment hands the pattern is dot(pixel - B, segDir), so its ISO-LINES run perpendicular
// to THAT segment. Two segments at a joint therefore disagree about a pixel's arc by
// lateral * 2sin(theta/2): zero on the centre line, largest at the stroke edge, opposite in sign on
// the two sides. A dash crossing a seam does not shift, it KINKS.
//   0 - the plain per-segment frame.
//   1 - near each end, measure the pattern's arc against the joint's angle BISECTOR instead. Both
//       segments then produce the same arc for the same pixel and the step is gone.
#ifndef CURVE_PATTERN_BISECTOR
#define CURVE_PATTERN_BISECTOR 1
#endif

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<float> PatternPosition : register(t1);

// Exactly one slot of this is read - [TotalCurveCount], the chain's centre total, which is the
// whole clamp bound now that the pattern window is chain-global. Wave-uniform, so it scalarises
// into one cached load instead of an interpolant written five times per segment.
StructuredBuffer<uint>  PatternOffsets  : register(t2);

static const float CurveInvSqrt2 = 0.70710678118;
static const float CurveSdfInside = -1e30;
// max()ed into the stroke, so this erases the body outright - a patterned curve with no centre
// anywhere near it draws nothing, which is a gap in the pattern, not a solid run.
static const float CurveSdfOutside = 1e30;

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

#if CURVE_PATTERN_BISECTOR

// Segment-local arc for the PATTERN only. The body, the caps, the join and the terminus test keep
// the plain segment arc, which is the right coordinate for them.
//
// The whole correction is lateral * tan(theta/2). Two properties make it safe to bolt on:
//   - identically zero on the centre line, so phase ALONG the curve, spacing, dash length and the
//     shrink-to-fit sizing are all untouched; only the disagreement that grows toward the stroke
//     edge is straightened out.
//   - both segments at a joint evaluate to the same number for the same pixel, which is the point.
//
// The pinned band at each end is not a tuning constant: two segments meeting at a turn of theta
// overlap - and their miter wedge sticks out - by halfWidth * tan(theta/2) along the arc, and
// |arcShear| IS that tan(theta/2). So the band is as wide as the shared zone and no wider, and it
// collapses to nothing on a straight run, where there is nothing to reconcile.
//
// Side effect, same family as the shrink-to-fit one: inside a band the arc gradient is 1/cos(theta/2)
// rather than 1, so dash ends antialias over a slightly narrower band at sharp joints.
float CurvePatternArc(
    float  localArc,
    float  lateral,
    float  segmentLength,
    float  halfWidth,
    float2 arcShear)
{
    const float wantB = halfWidth * abs(arcShear.x);
    const float wantC = halfWidth * abs(arcShear.y);

    // Too short to hold both bands (fat stroke, coarse sampling, hairpin): scale them down TOGETHER
    // so the sharper joint keeps the larger share, and always leave a fifth of the segment to ramp
    // across. Letting the bands meet would turn the ramp into a step and just move the seam to the
    // middle of the segment.
    const float want   = wantB + wantC;
    const float allow  = segmentLength * 0.8;
    const float shrink = (want > allow) ? (allow / max(want, 1e-6)) : 1.0;

    const float bandB = wantB * shrink;
    const float bandC = wantC * shrink;

    const float ramp = max(segmentLength - bandB - bandC, 1e-3);
    const float w    = saturate((localArc - bandB) / ramp);

    return localArc + lateral * lerp(arcShear.x, arcShear.y, w);
}

#endif

#if CURVE_PATTERN_SHRINK_TO_FIT
// slot0/slot1 are absolute slots in the flat pattern array. The look-back is refused only at the
// very first centre in the SCENE now, not at the start of each curve's slice - everywhere else the
// span is measured against the previous centre along the chain, whichever curve owns it. Same
// result the one-slot slice widening used to buy, without the widening.
float CurvePatternCentreSpan(int slot0, int slot1, float c0, float c1)
{
    if (slot1 != slot0)
    {
        return abs(c1 - c0);
    }
    if (slot0 > 0)
    {
        return abs(c0 - PatternPosition[uint(slot0 - 1)]);
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

// patternArc is the curve-global screen arc in the PATTERN's frame: the plain segment arc when
// CURVE_PATTERN_BISECTOR is off, the bisector-sheared one when it is on.
float CurvePatternSDF(
    float  lateral,
    float  patternArc,
    float  patternCoord,
    float  dashLength,
    float  halfWidth,
    uint   capCapJoin)
{
    // SOLID is spacing <= 0 and nothing else. It used to be tested as "this curve owns no pattern
    // centres", which meant the same thing only while every curve started its own pattern at its
    // own arc 0 and therefore always owned at least one. On the chain-global grid a curve shorter
    // than the gap between two grid points legitimately owns NONE - at spacing 2.0 most curves in
    // a scene do - and reading that as solid turns those curves into full-width strokes. Spacing no
    // longer reaches the pixel shader, so the test is a flag the vertex shader sets from
    // Spacing > 0.0 - a NaN spacing therefore still reads as solid.
    if (!IsPatterned(capCapJoin))
    {
        return CurveSdfInside;
    }

    // --- the window is the whole chain ---------------------------------------------------------
    // One chain (see curve_vs.hlsl) means PatternPosition is one dense, sorted run covering every
    // curve, so the only slots that do not exist are off its two ends and [0, lastSlot] is the
    // complete bound. The per-curve window - a slice plus one slot of widening at each end so a
    // dash could reach across a joint - is a subset of what this clamp already allows, so it and
    // its two interpolants go together.
    //
    // Given up: an out-of-range coordinate used to clamp to this curve's own edge, one centre away,
    // so a bad coordinate drew a slightly misplaced dash. It now clamps to the end of the scene,
    // where nothing is near patternArc, so a GROSS error drops the dash instead. Small errors still
    // land on a neighbouring centre, the array being dense and sorted. Measured: with the arc
    // correct the old clamp never fired at all; it only ever damped the noperspective world-arc
    // error, which is better fixed than damped.
    const int lastSlot = int(PatternOffsets[TotalCurveCount]) - 1;

    // Patterned, but the chain holds no centre at all - it is all gap, NOT solid.
    if (lastSlot < 0)
    {
        return CurveSdfOutside;
    }

    // patternCoord already IS the slot coordinate - the vertex shader folded the divide by spacing
    // and the grid-index-to-slot bias into it.
    const int slot0 = clamp(int(floor(patternCoord)), 0, lastSlot);
    const int slot1 = min(slot0 + 1, lastSlot);

    const float c0 = PatternPosition[uint(slot0)];
    const float c1 = PatternPosition[uint(slot1)];

    const float a0 = patternArc - c0;
    const float a1 = patternArc - c1;
    float arcDist = (abs(a0) <= abs(a1)) ? a0 : a1;
    // BEHAVIOUR CHANGE, the only one here. This read `totalDistance > arcDist`, comparing a WORLD
    // arc length against a SCREEN-pixel offset from a centre - the unit mismatch in
    // curve_renderer.md. The world arc is gone and the slot coordinate replacing it is no more
    // comparable to a pixel offset, so this is written the way dot_ps.hlsl already writes it:
    // arcDist is signed from the dash's centre, so its sign picks the end. Only visible where
    // cap_front != cap_back.
    uint cap = (arcDist < 0.0) ? FrontCap(capCapJoin) : BackCap(capCapJoin);

#if CURVE_PATTERN_SHRINK_TO_FIT
    arcDist *= CurvePatternArcScale(
        CurvePatternCentreSpan(slot0, slot1, c0, c1),
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

    // The pattern is the one consumer that spans segments, so it gets the seam-consistent arc.
#if CURVE_PATTERN_BISECTOR
    const float patternArc = input.ScreenArcBegin + CurvePatternArc(
        localArc, lateral, segmentLength, halfWidth, input.ArcShear);
#else
    const float patternArc = currentArc;
#endif

    sdf = max(sdf, CurvePatternSDF(
        lateral,
        patternArc,
        input.ColorPattern.w,
        input.DashLength,
        halfWidth,
        input.CapCapJoin));

    if (sdf > 0.5) discard;
    return float4(input.ColorPattern.rgb, saturate(0.5 - sdf));
}
