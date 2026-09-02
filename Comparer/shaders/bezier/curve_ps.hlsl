#include "curve_common.hlsli"

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
        const float currentArc = input.ScreenArcBegin + localArc;

        const int lastPattern = int(input.PatternRange.y) - 1;
        const int i0 = clamp(int(floor(input.TotalDistance / max(input.Spacing, 1e-6))), 0, lastPattern);
        const int i1 = min(i0 + 1, lastPattern);

        const float c0 = PatternPosition[input.PatternRange.x + uint(i0)];
        const float c1 = PatternPosition[input.PatternRange.x + uint(i1)];

        const float a0 = currentArc - c0;
        const float a1 = currentArc - c1;
        const float arcDist = (abs(a0) <= abs(a1)) ? a0 : a1;

        if (input.Pattern == CurvePatternDot) {
            finalD = max(finalD, length(float2(lateral, arcDist)) - input.DashLength);
        } else {
            finalD = max(finalD, abs(arcDist) - input.DashLength);
        }
    }
    
    if (finalD > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - finalD));
}