#include "curve_common.hlsli"

// Squeeze the dash along the curve so it never reaches into its neighbor as the curve recedes.
#ifndef CURVE_PATTERN_SHRINK_TO_FIT
#define CURVE_PATTERN_SHRINK_TO_FIT 1
#endif

// Clamp the pattern arc to the segment's OWN arc range at its joints. The quad reaches well past the
// segment's ends (a miter, or a half-width of overshoot), and there the segment-local arc keeps
// counting straight on while the curve turns away - so those pixels read the pattern at arcs this
// segment never covers. Worst when the curve heads toward the camera: the segment is ~0 px long on
// screen but its quad is a full stroke wide, so nearly all of it evaluates the pattern at somebody
// else's arc and a segment lying wholly inside a dash only fills a stripe of its quad. Clamped, a
// pixel can never read past where this segment starts or where the next one begins: a segment inside
// a dash fills its whole quad, a joint in a gap stays a gap. Chain ends (no neighbour) are not
// clamped, so the dash and the terminus cap shape the curve's ends exactly as before.
#ifndef CURVE_PATTERN_CLAMP_TO_SEGMENT
#define CURVE_PATTERN_CLAMP_TO_SEGMENT 1
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

// Screen arc of every dash center as a compensated pair (hi, lo), one sorted run per chain, each starting
// again at arc 0. The LAST center of each chain has the sign bit of its hi set (curve_pattern_calc.hlsl):
// the slot after a flagged one belongs to the next chain, and so does the slot before a chain's first
// center. That is how a pixel stays inside its own chain without being handed the chain's slot range.
//
// Centers are only ever used relative to something else on the same chain - the segment's start arc, or
// the neighbouring center - and those differences are taken pairwise, so they stay as precise as the
// (small) difference itself however long the chain gets.
StructuredBuffer<float2> PatternPosition : register(t1);

bool IsChainLastCenter(float2 stored) { return (asuint(stored.x) & 0x80000000u) != 0u; }
float2 CenterArc(float2 stored) { return float2(asfloat(asuint(stored.x) & 0x7FFFFFFFu), stored.y); }

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
// one, so it looks back instead - unless the center before it is another chain's last, i.e. this is
// the chain's only center (or slot 0), in which case there is nothing to measure against.
float PatternCenterSpan(int slot0, bool isLast0, float2 center0, float2 center1) {
    if (!isLast0)
        return abs(PairDiff(center1, center0));
    if (slot0 > 0) {
        float2 previous = PatternPosition[uint(slot0 - 1)];
        if (!IsChainLastCenter(previous))
            return abs(PairDiff(center0, previous));
    }
    return 0.0;
}

float PatternArcScale(float centerSpan, float dashLength, float halfWidth, uint cap) {
    float elementLength = dashLength + ((cap == CurveCapButt) ? 0.0 : 2.0 * halfWidth);
    float budget = centerSpan * CURVE_PATTERN_FILL;
    return (budget > 1e-6) ? max(1.0, elementLength / budget) : 1.0;
}
#endif

// segmentArc: the screen arc in the pattern's frame, relative to arcBegin, the segment's start arc (a pair).
// patternSlot + floor(patternCoord): the slot, see curve_vs.hlsl.
float PatternSDF(float lateral, float segmentArc, float2 arcBegin, uint patternSlot, float patternCoord, float dashLength, float halfWidth, uint capCapJoin) {
    // The coordinate never reaches below the chain's first slot (curve_vs.hlsl), so there is no lower
    // bound to apply; the max() only keeps a stray negative value from wrapping around as an unsigned index.
    int slot0 = max(int(patternSlot) + int(floor(patternCoord)), 0);

    float2 stored0 = PatternPosition[uint(slot0)];
    bool isLast0 = IsChainLastCenter(stored0);
    float2 center0 = CenterArc(stored0);
    // After the chain's last center comes another chain's first one: don't read it, pair the center with itself.
    float2 center1 = isLast0 ? center0 : CenterArc(PatternPosition[uint(slot0 + 1)]);

    // The pixel's offset from each center: the pixel's arc from the segment start, minus the center's -
    // the latter taken pairwise, as it is the difference of two arcs from the chain's start.
    float arc0 = segmentArc - PairDiff(center0, arcBegin);
    float arc1 = segmentArc - PairDiff(center1, arcBegin);
    float arcDist = (abs(arc0) <= abs(arc1)) ? arc0 : arc1;
    // arcDist is signed from the dash's center, so its sign picks the end (as in dot_ps.hlsl).
    uint cap = (arcDist < 0.0) ? FrontCap(capCapJoin) : BackCap(capCapJoin);

#if CURVE_PATTERN_SHRINK_TO_FIT
    arcDist *= PatternArcScale(PatternCenterSpan(slot0, isLast0, center0, center1), dashLength, halfWidth, cap);
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

    // The pattern is the one thing that spans segments, so it gets the seam-consistent arc. Measured
    // from the segment start (input.ScreenArcBegin), which keeps it a small, precise number; PatternSDF
    // relates it to the dash centers pairwise.
#if CURVE_PATTERN_BISECTOR
    float segmentArc = PatternArc(localArc, lateral, segmentLength, halfWidth, input.ArcShear);
#else
    float segmentArc = localArc;
#endif

#if CURVE_PATTERN_CLAMP_TO_SEGMENT
    // [0, segmentLength] from the segment start is this segment's arc range: B is where it starts, C
    // where the next segment begins. Only clamped at ends that have a neighbour.
    {
        const float arcLo = (input.Neighbors.x != 0u) ? 0.0 : -1e30;
        const float arcHi = (input.Neighbors.y != 0u) ? segmentLength : 1e30;
        segmentArc = clamp(segmentArc, arcLo, arcHi);
    }
#endif
    sdf = max(sdf, PatternSDF(lateral, segmentArc, input.ScreenArcBegin, input.PatternSlot, input.ColorPattern.w,
                              input.DashLength, halfWidth, input.CapCapJoin));

    if (sdf > 0.5) discard;
    return float4(input.ColorPattern.rgb, saturate(0.5 - sdf));
}
