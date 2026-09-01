// Step 5b of the curve pipeline: shade the stroke body, then cut the pattern out of it.
//
// The body works in SEGMENT-LOCAL coordinates - x across the stroke, y along it with 0 at B and
// l_CB at C - which is what makes the rounded ends land in the right place at every join.
//
// The pattern works in the curve's own arc-length frame: the pixel's curve-global screen arc length
// is ScreenArcBegin + the segment-local y, and every pattern center in PatternPosition is stored in
// the same units. Because that frame bends with the curve, it suits marks that are meant to follow
// the curve (dashes, dots) and would warp a rigid shape - which is why the glyph variant of this
// renderer is a separate implementation and not a switch in here.
//
// Cap and Join arrive in input.CapJoin but are not acted on yet: ends are always rounded and the
// vertex shader always builds the same join geometry.
#include "curve_common.hlsli"

// Screen-space arc length of every pattern center, packed for all curves back to back.
// PatternRange.x is where this curve's block starts inside it.
StructuredBuffer<float> PatternPosition : register(t1);

float4 main(CurveVSOutput input) : SV_Target {
    const float widthPixel    = input.SDF.z;
    const float lateral       = input.SDF.x; // Signed distance from the centre line, px
    const float localArc      = input.SDF.y; // Segment-local, 0 at B
    const float segmentLength = input.SDF.w;

    // --- stroke body + join/cap rounding -------------------------------------------
    const float2 pCoord = float2(abs(lateral), localArc);
    float roundD;
    if (pCoord.y < 0.0) {
        roundD = length(pCoord) - widthPixel;
    } else if (pCoord.y > segmentLength) {
        roundD = length(pCoord - float2(0.0, segmentLength)) - widthPixel;
    } else {
        roundD = pCoord.x - widthPixel;
    }

    float finalD = roundD;

    // --- pattern -------------------------------------------------------------------
    if (input.Pattern != CurvePatternSolid && input.PatternRange.y > 0u) {
        // The curve-global screen arc length at this pixel. ScreenArcBegin is constant over the
        // segment, so this is exact rather than an interpolation of two cumulative values.
        const float currentArc = input.ScreenArcBegin + localArc;

        // Pattern k of this curve sits at k * Spacing in WORLD arc length, so the world distance
        // picks the candidate index. World and screen arc length are both monotonic along the
        // curve but not proportional, and TotalDistance is interpolated in screen space, so the
        // index can land one off near a boundary - test the neighbour too and keep whichever
        // center is actually nearer in screen space. That removes the seam without a search.
        const int lastPattern = int(input.PatternRange.y) - 1;
        const int i0 = clamp(int(floor(input.TotalDistance / max(input.Spacing, 1e-6))), 0, lastPattern);
        const int i1 = min(i0 + 1, lastPattern);

        const float c0 = PatternPosition[input.PatternRange.x + uint(i0)];
        const float c1 = PatternPosition[input.PatternRange.x + uint(i1)];

        const float a0 = currentArc - c0;
        const float a1 = currentArc - c1;
        const float arcDist = (abs(a0) <= abs(a1)) ? a0 : a1;

        if (input.Pattern == CurvePatternDot) {
            // A disc of radius DashLength centred on the pattern, intersected with the stroke.
            // DashLength is uploaded as the stroke half-width for Dot, so it comes out round.
            finalD = max(finalD, length(float2(lateral, arcDist)) - input.DashLength);
        } else {
            // Dash: keep only what is within DashLength of a pattern center, along the curve.
            finalD = max(finalD, abs(arcDist) - input.DashLength);
        }
    }

    // Antialias over the last pixel instead of hard-discarding - this is what the AlphaBlend
    // state on the draw pipeline is for.
    if (finalD > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - finalD));
}