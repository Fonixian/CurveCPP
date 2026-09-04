#include "solid_common.hlsli"

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

// --- end caps -------------------------------------------------------- --------------
float CurveCapSDF(float2 coord, float spanLength, float halfWidth, uint cap)
{
    const float body      = CurveBodySDF(coord, halfWidth);
    const float overshoot = CurveEndOvershoot(coord, spanLength);
    float d;
    switch (cap)
    {
        case CurveCapButt: d = max(body, overshoot); break;
        case CurveCapSquare: d = max(body, overshoot - halfWidth); break;
        case CurveCapTriangleOut: d = max(body, (coord.x + overshoot - halfWidth) * rsqrt(2.0)); break;
        case CurveCapTriangleIn: d = max(body, (overshoot - coord.x) * rsqrt(2.0)); break;
        default: d = CurveRoundEndSDF(coord, spanLength, halfWidth); break;
    }
    return d;
}

// --- joins -------------------------------------------------------------------------
float CurveJoinSDF(float2 coord, float spanLength, float halfWidth, uint join)
{
    if (join == CurveJoinRound)
        return CurveRoundEndSDF(coord, spanLength, halfWidth);
    return CurveBodySDF(coord, halfWidth);
}

// -----------------------------------------------------------------------------------

float4 main(SolidVSOutput input) : SV_Target
{
    const float lateral       = input.SDF.x;
    const float localArc      = input.SDF.y;
    const float halfWidth     = input.SDF.z;
    const float segmentLength = input.SDF.w;

    const float2 coord = float2(abs(lateral), localArc);
    
    const bool pastBegin = localArc < 0.0;
    const bool pastEnd   = localArc > segmentLength;
    const bool atTerminus = (pastBegin && input.Neighbors.x == 0u)
                         || (pastEnd   && input.Neighbors.y == 0u);

    const float sdf = atTerminus
        ? CurveCapSDF(coord, segmentLength, halfWidth, pastBegin ? FrontCap(input.CapCapJoin) : BackCap(input.CapCapJoin))
        : CurveJoinSDF(coord, segmentLength, halfWidth, Join(input.CapCapJoin));

    if (sdf > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - sdf));
}