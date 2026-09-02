// Step 5b of the curve pipeline: shade one pixel of the stroke.
//
// Everything happens in the segment-local SDF frame the vertex shader set up:
//   coord.x = |lateral|  distance from the centre line, in pixels
//   coord.y = localArc   distance along the segment from point B, in pixels (0 at B, segmentLength at C)
//
// A pixel is "past an end" when coord.y < 0 (before B) or coord.y > segmentLength (after C). Whether
// that end is a curve terminus or a neighbouring segment is decided from the curve-global screen arc
// length: outside [0, ScreenArcEnd] there is no neighbour, so the cap rule applies; anywhere else the
// join rule does.
//
// Each of the three stroke features is a standalone SDF function, so they can be combined (and
// tested) independently:
//   CurveCapSDF     - end treatment at a curve terminus
//   CurveJoinSDF    - end treatment where two segments meet
//   CurvePatternSDF - dash/dot mask, intersected with whichever of the two produced the body
#include "curve_common.hlsli"

StructuredBuffer<float> PatternPosition : register(t1);

static const float CurveInvSqrt2 = 0.70710678118;

// Returned by CurvePatternSDF for a solid stroke: "deep inside", so max() leaves the body untouched.
static const float CurveSdfInside = -1e30;

// --- shared building blocks --------------------------------------------------------

// Stroke with no end treatment at all: an infinite band of half-width `halfWidth`.
float CurveBodySDF(float2 coord, float halfWidth)
{
    return coord.x - halfWidth;
}

// How far past the nearer end of the segment the pixel sits, in pixels.
// <= 0 anywhere inside the segment body, > 0 in the cap/join region beyond B or C.
float CurveEndOvershoot(float2 coord, float segmentLength)
{
    return max(-coord.y, coord.y - segmentLength);
}

// Semicircle of radius `halfWidth` around the nearer end point.
float CurveRoundEndSDF(float2 coord, float segmentLength, float halfWidth)
{
    const float overshoot = CurveEndOvershoot(coord, segmentLength);
    return (overshoot > 0.0)
        ? length(float2(coord.x, overshoot)) - halfWidth
        : CurveBodySDF(coord, halfWidth);
}

// --- end caps ----------------------------------------------------------------------
// The vertex shader already extends the strip by one half-width past a terminus, so every cap
// shape below has the geometry it needs. `cap` is a CurveCap* constant.
float CurveCapSDF(float2 coord, float segmentLength, float halfWidth, uint cap)
{
    const float body      = CurveBodySDF(coord, halfWidth);
    const float overshoot = CurveEndOvershoot(coord, segmentLength);

    if (cap == CurveCapButt)
    {
        // Flat cut exactly at the end point.
        return max(body, overshoot);
    }
    if (cap == CurveCapSquare)
    {
        // Flat cut one half-width past the end point.
        return max(body, overshoot - halfWidth);
    }
    if (cap == CurveCapTriangleOut)
    {
        // Arrowhead: apex on the centre line, one half-width past the end point.
        return max(body, (coord.x + overshoot - halfWidth) * CurveInvSqrt2);
    }
    if (cap == CurveCapTriangleIn)
    {
        // Notch: the corners reach the end point, the centre line is cut back by one half-width.
        return max(body, (overshoot - coord.x) * CurveInvSqrt2);
    }

    // CurveCapRound
    return CurveRoundEndSDF(coord, segmentLength, halfWidth);
}

// --- joins -------------------------------------------------------------------------
// `join` is a CurveJoin* constant.
float CurveJoinSDF(float2 coord, float segmentLength, float halfWidth, uint join)
{
    if (join == CurveJoinRound)
    {
        return CurveRoundEndSDF(coord, segmentLength, halfWidth);
    }

    // CurveJoinSquare: the miter/bevel wedge is built by the vertex shader (the strip corners are
    // pushed out to meet the neighbouring segment), so here only the lateral band is needed.
    return CurveBodySDF(coord, halfWidth);
}

// --- dash / dot pattern ------------------------------------------------------------
// Mask that keeps the pixel only when it is close enough to a pattern centre. Intersect it with the
// body SDF via max(). `pattern` is a CurvePattern* constant.
//
// lateral      signed distance from the centre line, in pixels
// currentArc   curve-global screen arc length of this pixel, in pixels
// totalDistance cumulative WORLD arc length, used to pick which pattern centres to test
float CurvePatternSDF(
    float lateral,
    float currentArc,
    float totalDistance,
    float spacing,
    uint2 patternRange,
    float dashLength,
    uint  pattern)
{
    if (pattern == CurvePatternSolid || patternRange.y == 0u)
    {
        return CurveSdfInside;
    }

    // World distance / spacing gives the index of the nearest pattern centre; test it and its
    // successor so the pattern stays continuous across the boundary between the two.
    const int lastPattern = int(patternRange.y) - 1;
    const int i0 = clamp(int(floor(totalDistance / max(spacing, 1e-6))), 0, lastPattern);
    const int i1 = min(i0 + 1, lastPattern);

    const float c0 = PatternPosition[patternRange.x + uint(i0)];
    const float c1 = PatternPosition[patternRange.x + uint(i1)];

    const float a0 = currentArc - c0;
    const float a1 = currentArc - c1;
    const float arcDist = (abs(a0) <= abs(a1)) ? a0 : a1;

    if (pattern == CurvePatternDot)
    {
        // Disc around the centre.
        return length(float2(lateral, arcDist)) - dashLength;
    }

    // CurvePatternDash: band of arc length around the centre, full stroke width.
    return abs(arcDist) - dashLength;
}

// -----------------------------------------------------------------------------------

float4 main(CurveVSOutput input) : SV_Target
{
    const float lateral       = input.SDF.x;
    const float localArc      = input.SDF.y;
    const float halfWidth     = input.SDF.z;
    const float segmentLength = input.SDF.w;

    const float2 coord = float2(abs(lateral), localArc);

    // Curve-global screen arc length of this pixel.
    const float currentArc = input.ScreenArcBegin + localArc;

    // Outside the curve's arc range there is no neighbouring segment to join to.
    const bool atTerminus = (currentArc < 0.0) || (currentArc > input.ScreenArcEnd);

    float sdf = atTerminus
        ? CurveCapSDF(coord, segmentLength, halfWidth, input.CapJoin.x)
        : CurveJoinSDF(coord, segmentLength, halfWidth, input.CapJoin.y);

    sdf = max(sdf, CurvePatternSDF(
        lateral,
        currentArc,
        input.TotalDistance,
        input.Spacing,
        input.PatternRange,
        input.DashLength,
        input.Pattern));

    if (sdf > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - sdf));
}
