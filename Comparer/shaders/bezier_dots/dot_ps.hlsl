#include "dot_common.hlsli"

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

// A dot is a zero-length dash: the front cap shapes the half facing the curve's start, the back cap
// the half facing its end.
float4 main(DotVSOutput input) : SV_Target {
    float lateral = input.SDF.x;
    float along = input.SDF.y;
    float halfWidth = input.SDF.z;

    float2 coord = float2(abs(lateral), along);
    uint cap = (along < 0.0) ? FrontCap(input.CapCapJoin) : BackCap(input.CapCapJoin);

    float sdf;
    if (cap == CurveCapRound)
        sdf = length(coord) - halfWidth;
    else
        sdf = max(coord.x - halfWidth, CurveCapSDF(coord, abs(along), halfWidth, cap));

    if (sdf > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - sdf));
}
