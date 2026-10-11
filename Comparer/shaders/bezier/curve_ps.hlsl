#include "curve_common.hlsli"

// Squeeze the dash along the curve so it never reaches into its neighbor as the curve recedes.
#ifndef CURVE_PATTERN_SHRINK_TO_FIT
#define CURVE_PATTERN_SHRINK_TO_FIT 1
#endif

// How much of the center-to-center span one dash may occupy.
#ifndef CURVE_PATTERN_FILL
#define CURVE_PATTERN_FILL 0.95
#endif

// The segment arc's iso-lines run perpendicular to the segment, so two segments at a joint disagree
// about a pixel's arc by lateral * 2sin(theta/2) and a dash crossing the joint kinks.
//   0 - plain segment arc.
//   1 - near each end, measure the pattern's arc against the joint's angle bisector instead, so
//       both segments agree.
#ifndef CURVE_PATTERN_BISECTOR
#define CURVE_PATTERN_BISECTOR 1
#endif

StructuredBuffer<float> PatternPosition : register(t1); // screen arc of every dash center, one sorted run per chain

float CurveCapSDF(float2 coord, float overshoot, float halfWidth, uint cap) {
    float d;
    switch (cap) {
        case CurveCapButt:
            d = overshoot;
            break;
        case CurveCapSquare:
            d = overshoot - halfWidth;
            break;
        case CurveCapTriangleOut:
            d = (coord.x + overshoot - halfWidth) * rsqrt(2.0);
            break;
        case CurveCapTriangleIn:
            d = (overshoot - coord.x) * rsqrt(2.0);
            break;
    }
    return d;
}

#if CURVE_PATTERN_BISECTOR
// The segment arc sheared by lateral * tan(theta/2) near each end. It is zero on the center line,
// so phase, spacing, dash length and shrink-to-fit are untouched along the curve.
//
// The band at each end is halfWidth * tan(theta/2) wide: exactly where two segments meeting at a
// turn of theta overlap (and their miter wedge sticks out). On a straight run it is empty.
// Inside a band the arc gradient is 1 / cos(theta/2), so dash ends antialias slightly narrower there.
float PatternArc(float localArc, float lateral, float segmentLength, float halfWidth, float2 arcShear) {
    float wantStart = halfWidth * abs(arcShear.x);
    float wantEnd = halfWidth * abs(arcShear.y);

    // Too short for both bands: scale them down together and always leave a fifth of the segment to
    // ramp across - bands that meet would just move the seam to the middle.
    float want = wantStart + wantEnd;
    float allow = segmentLength * 0.8;
    float shrink = (want > allow) ? (allow / max(want, 1e-6)) : 1.0;

    float bandStart = wantStart * shrink;
    float bandEnd = wantEnd * shrink;

    float ramp = max(segmentLength - bandStart - bandEnd, 1e-3);
    float w = saturate((localArc - bandStart) / ramp);

    return localArc + lateral * lerp(arcShear.x, arcShear.y, w);
}
#endif

#if CURVE_PATTERN_SHRINK_TO_FIT
// Distance between the two centers around this pixel. At the chain's last center there is no next
// one, so it looks back instead - but never past the chain's first slot, which is another stroke.
float PatternCenterSpan(int slot0, int slot1, float center0, float center1, int firstSlot) {
    if (slot1 != slot0)
        return abs(center1 - center0);
    if (slot0 > firstSlot)
        return abs(center0 - PatternPosition[uint(slot0 - 1)]);
    return 0.0;
}

float PatternArcScale(float centerSpan, float dashLength, float halfWidth, uint cap) {
    float elementLength = dashLength + ((cap == CurveCapButt) ? 0.0 : 2.0 * halfWidth);
    float budget = centerSpan * CURVE_PATTERN_FILL;
    return (budget > 1e-6) ? max(1.0, elementLength / budget) : 1.0;
}
#endif

// patternArc: the screen arc in the pattern's frame. patternCoord: the slot coordinate from the vertex shader.
float PatternSDF(float lateral, float patternArc, float patternCoord, float dashLength, float halfWidth, uint capCapJoin, uint2 patternSlots) {
    int firstSlot = int(patternSlots.x);
    int lastSlot = int(patternSlots.y) - 1;

    // The chain has no center at all (too short for one): all gap, so erase the body.
    if (lastSlot < firstSlot) return 1e30;

    // The clamp only catches rounding at the edges, e.g. a coordinate a hair under the chain's first
    // slot that would floor into the previous stroke.
    int slot0 = clamp(int(floor(patternCoord)), firstSlot, lastSlot);
    int slot1 = min(slot0 + 1, lastSlot);

    float center0 = PatternPosition[uint(slot0)];
    float center1 = PatternPosition[uint(slot1)];

    float arc0 = patternArc - center0;
    float arc1 = patternArc - center1;
    float arcDist = (abs(arc0) <= abs(arc1)) ? arc0 : arc1;
    // arcDist is signed from the dash's center, so its sign picks the end (as in dot_ps.hlsl).
    uint cap = (arcDist < 0.0) ? FrontCap(capCapJoin) : BackCap(capCapJoin);

#if CURVE_PATTERN_SHRINK_TO_FIT
    arcDist *= PatternArcScale(PatternCenterSpan(slot0, slot1, center0, center1, firstSlot), dashLength, halfWidth, cap);
#endif

    // The dash as a stroke span [0, dashLength] with the cap at both ends.
    float2 coord = float2(abs(lateral), arcDist + dashLength * 0.5);
    float sdf = coord.x - halfWidth;
    float overshoot = max(-coord.y, coord.y - dashLength);

    if (cap == CurveCapRound)
        sdf = (overshoot > 0.0) ? length(float2(coord.x, overshoot)) - halfWidth : sdf;
    else
        sdf = max(sdf, CurveCapSDF(coord, overshoot, halfWidth, cap));
    return sdf;
}

float4 main(PatternedVSOutput input) : SV_Target {
    float lateral = input.SDF.x;
    float localArc = input.SDF.y;
    float halfWidth = input.SDF.z;
    float segmentLength = input.SDF.w;

    float2 coord = float2(abs(lateral), localArc);

    bool2 pastEnd = bool2(localArc < 0.0, localArc > segmentLength);
    uint endCap = pastEnd.x ? FrontCap(input.CapCapJoin) : BackCap(input.CapCapJoin);
    bool atEndCap = any(pastEnd && (input.Neighbors == 0u));

    float sdf = coord.x - halfWidth;
    float overshoot = max(-coord.y, coord.y - segmentLength);

    if ((atEndCap && endCap == CurveCapRound) || (!atEndCap && Join(input.CapCapJoin) == CurveJoinRound))
        sdf = (overshoot > 0.0) ? length(float2(coord.x, overshoot)) - halfWidth : sdf;
    else if (atEndCap)
        sdf = max(sdf, CurveCapSDF(coord, overshoot, halfWidth, endCap));

    // The pattern is the one thing that spans segments, so it gets the seam-consistent arc.
#if CURVE_PATTERN_BISECTOR
    float patternArc = input.ScreenArcBegin + PatternArc(localArc, lateral, segmentLength, halfWidth, input.ArcShear);
#else
    float patternArc = input.ScreenArcBegin + localArc;
#endif
    sdf = max(sdf, PatternSDF(lateral, patternArc, input.ColorPattern.w, input.DashLength, halfWidth, input.CapCapJoin, input.PatternSlots));

    if (sdf > 0.5) discard;
    return float4(input.ColorPattern.rgb, saturate(0.5 - sdf));
}
