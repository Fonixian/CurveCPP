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
//   CurveCapSDF     - end treatment at an end of the stroke
//   CurveJoinSDF    - end treatment where two segments meet
//   CurvePatternSDF - dash/dot mask, intersected with whichever of the two produced the body
//
// The cap is a property of a stroke END, so which ends exist depends on the pattern:
//   Solid - one end at each curve terminus.
//   Dash  - both ends of every dash, plus the curve terminus (which clips the outermost dashes).
//   Dot   - none. A dot is a disc; capping the terminus would only slice the last one, so the
//           terminus falls back to the plain lateral band and the disc does all the shaping.
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

// How far past the nearer end of the span the pixel sits, in pixels.
// <= 0 anywhere inside the span, > 0 in the cap/join region beyond either end.
float CurveEndOvershoot(float2 coord, float spanLength)
{
    return max(-coord.y, coord.y - spanLength);
}

// Semicircle of radius `halfWidth` around the nearer end point.
float CurveRoundEndSDF(float2 coord, float spanLength, float halfWidth)
{
    const float overshoot = CurveEndOvershoot(coord, spanLength);
    return (overshoot > 0.0)
        ? length(float2(coord.x, overshoot)) - halfWidth
        : CurveBodySDF(coord, halfWidth);
}

// --- end caps ----------------------------------------------------------------------
// Caps both ends of a span that runs from coord.y == 0 to coord.y == spanLength. Used twice: once
// over the whole segment (the curve's terminus) and once over a single dash, in the dash's own
// frame. `cap` is a CurveCap* constant.
//
// The vertex shader extends the strip by one half-width past a terminus, and a dash always has
// stroke on both sides of it, so every shape below has the geometry it needs.
float CurveCapSDF(float2 coord, float spanLength, float halfWidth, uint cap)
{
    const float body      = CurveBodySDF(coord, halfWidth);
    const float overshoot = CurveEndOvershoot(coord, spanLength);

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
    return CurveRoundEndSDF(coord, spanLength, halfWidth);
}

// Does this pattern have stroke ends for the cap to shape? Solid and Dash do; Dot does not, because
// a dot is bounded by its own disc in every direction.
bool CurveCapAppliesTo(uint pattern)
{
    return pattern == CurvePatternSolid || pattern == CurvePatternDash;
}

// --- joins -------------------------------------------------------------------------
// `join` is a CurveJoin* constant.
float CurveJoinSDF(float2 coord, float spanLength, float halfWidth, uint join)
{
    if (join == CurveJoinRound)
    {
        return CurveRoundEndSDF(coord, spanLength, halfWidth);
    }

    // CurveJoinSquare: the miter/bevel wedge is built by the vertex shader (the strip corners are
    // pushed out to meet the neighbouring segment), so here only the lateral band is needed.
    return CurveBodySDF(coord, halfWidth);
}

// Body of the stroke over this segment: capped where the curve ends, joined where it does not, and
// plain where the pattern has no use for a cap.
float CurveStrokeSDF(
    float2 coord,
    float  segmentLength,
    float  halfWidth,
    bool   atTerminus,
    uint   cap,
    uint   join,
    uint   pattern)
{
    if (!atTerminus)
    {
        return CurveJoinSDF(coord, segmentLength, halfWidth, join);
    }
    if (!CurveCapAppliesTo(pattern))
    {
        return CurveBodySDF(coord, halfWidth);
    }
    return CurveCapSDF(coord, segmentLength, halfWidth, cap);
}

// --- dash / dot pattern ------------------------------------------------------------
// Mask that keeps the pixel only when it is close enough to a pattern centre. Intersect it with the
// body SDF via max(). `pattern` is a CurvePattern* constant.
//
// lateral       signed distance from the centre line, in pixels
// currentArc    curve-global screen arc length of this pixel, in pixels
// totalDistance cumulative WORLD arc length, used to pick which pattern centres to test
// dashLength    HALF the length of one dash, or the radius of one dot, in pixels
float CurvePatternSDF(
    float  lateral,
    float  currentArc,
    float  totalDistance,
    float  spacing,
    uint2  patternRange,
    float  dashLength,
    float  halfWidth,
    uint   pattern,
    uint   cap)
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
        // Disc around the centre. No cap: the disc already closes the shape at both ends.
        return length(float2(lateral, arcDist)) - dashLength;
    }

    // CurvePatternDash: a span of stroke 2 * dashLength long, capped at BOTH ends with the curve's
    // cap style. Same CurveCapSDF as the terminus, just re-based so 0 is the start of the dash and
    // 2 * dashLength its end. (With CurveCapButt this reduces to abs(arcDist) - dashLength, the
    // plain rectangular dash.)
    return CurveCapSDF(float2(abs(lateral), arcDist + dashLength), 2.0 * dashLength, halfWidth, cap);
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

    float sdf = CurveStrokeSDF(
        coord,
        segmentLength,
        halfWidth,
        atTerminus,
        input.CapJoin.x,
        input.CapJoin.y,
        input.Pattern);

    sdf = max(sdf, CurvePatternSDF(
        lateral,
        currentArc,
        input.TotalDistance,
        input.Spacing,
        input.PatternRange,
        input.DashLength,
        halfWidth,
        input.Pattern,
        input.CapJoin.x));

    if (sdf > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - sdf));
}
