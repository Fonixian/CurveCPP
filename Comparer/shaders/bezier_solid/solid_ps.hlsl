#include "solid_common.hlsli"

float CurveCapSDF(float2 coord, float overshoot, float halfWidth, uint cap)
{
    float d;
    switch (cap)
    {
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

float4 main(SolidVSOutput input) : SV_Target
{
    const float lateral = input.SDF.x;
    const float localArc = input.SDF.y;
    const float halfWidth = input.SDF.z;
    const float segmentLength = input.SDF.w;

    const float2 coord = float2(abs(lateral), localArc);
    
    const bool pastBegin = localArc < 0.0;
    const bool pastEnd = localArc > segmentLength;
    const bool atEndcap = (pastBegin && input.Neighbors.x == 0u)
                         || (pastEnd && input.Neighbors.y == 0u);
    
    const uint endcap = pastBegin ? FrontCap(input.CapCapJoin) : BackCap(input.CapCapJoin);

    float sdf = coord.x - halfWidth;
    const float overshoot = max(-coord.y, coord.y - segmentLength);
    
    if ((atEndcap && endcap == CurveCapRound) || (!atEndcap && Join(input.CapCapJoin) == CurveJoinRound))
        sdf = (overshoot > 0.0) ? length(float2(coord.x, overshoot)) - halfWidth : sdf;
    else if (atEndcap)
        sdf = max(sdf, CurveCapSDF(coord, overshoot, halfWidth, endcap));
    
    if (sdf > 0.5)
        discard;
    return float4(input.Color.rgb, saturate(0.5 - sdf));
}