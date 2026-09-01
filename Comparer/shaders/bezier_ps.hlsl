struct VSOutputFinal {
    float4 Position : SV_Position;
    // x: lateral distance from the centre line (px, signed)
    // y: longitudinal distance from point B along the segment (px), SEGMENT-LOCAL
    // z: widthPixel   w: l_CB (segment length in px)
    noperspective float4 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    noperspective float TotalDistance : TEXCOORD1;      // Cumulative WORLD arc length
    nointerpolation float Spacing : TEXCOORD2;          // World-space pattern spacing
    nointerpolation float ScreenArcBegin : TEXCOORD3;   // Screen arc length at point B
    nointerpolation float DashLength : TEXCOORD4;       // Half-length of one dash, in pixels
    nointerpolation uint2 PatternRange : TEXCOORD5;     // x = first pattern index, y = count
    nointerpolation uint Style : TEXCOORD6;             // 0 = Solid, 1 = Segmented
};

// Screen-space arc length of every pattern center, packed for all curves back to back.
// PatternRange.x is where this curve's block starts inside it.
StructuredBuffer<float> PatternPosition : register(t1);

static const uint PatternStyleSolid = 0u;

float4 main(VSOutputFinal input) : SV_Target
{
    const float widthPixel    = input.SDF.z;
    const float localArc      = input.SDF.y; // Segment-local, 0 at B
    const float segmentLength = input.SDF.w;

    // --- stroke body + join/cap rounding -------------------------------------------
    // This part stays in segment-local coordinates: the two rounded ends are at 0 and l_CB.
    const float2 pCoord = float2(abs(input.SDF.x), localArc);
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
    if (input.Style != PatternStyleSolid && input.PatternRange.y > 0u)
    {
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

        // Longitudinal cut: keep only what is within DashLength of a pattern center.
        const float dLong = abs(arcDist) - input.DashLength;
        finalD = max(finalD, dLong);
    }

    // Antialias over the last pixel instead of hard-discarding - this is what the AlphaBlend
    // state on the draw pipeline is for.
    if (finalD > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - finalD));
}
