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

// Where this segment stops at one end, in localArc.
//
// The vertex shader hands over a loose triangle, so this is the only thing keeping a segment from
// painting a straight spike out of the far side of a corner. Returns how far PAST the joint the
// segment may still draw at this pixel's lateral position; negative means the cut bites into the
// segment instead, which is what the inside of a corner needs.
//
//   SolidJointBisector: the joint's angle bisector, localArc + lateral * shear == the joint's arc.
//     The neighbour keeps the other side of the same line, so a round join comes out as two exact
//     half-discs and a square one as an exact miter. Truncated at `reach`, which caps the miter
//     spike on the outer side of a sharp turn without letting go of the bisector on the inner side.
//   SolidJointFlat: fold, or an end the near plane cut - no bisector to share, stop flat at `reach`.
//   SolidTerminus: no cut at all; the cap SDF below is what bounds this end.
float SolidJointExtent(uint end, float signedShearedLateral, float reach)
{
    return (end == SolidJointBisector) ? min(signedShearedLateral, reach) : reach;
}

float4 main(SolidVSOutput input) : SV_Target
{
    const float lateral = input.SDF.x;
    const float localArc = input.SDF.y;
    const float halfWidth = input.SDF.z;
    const float segmentLength = input.SDF.w;

    const float reach = SolidJoinReach(input.CapCapJoin, halfWidth);

    if (input.Neighbors.x != SolidTerminus &&
        localArc < -SolidJointExtent(input.Neighbors.x, lateral * input.ArcShear.x, reach))
        discard;

    if (input.Neighbors.y != SolidTerminus &&
        localArc > segmentLength + SolidJointExtent(input.Neighbors.y, -lateral * input.ArcShear.y, reach))
        discard;

    const float2 coord = float2(abs(lateral), localArc);

    const bool pastBegin = localArc < 0.0;
    const bool pastEnd = localArc > segmentLength;
    const bool atEndcap = (pastBegin && input.Neighbors.x == SolidTerminus)
                         || (pastEnd && input.Neighbors.y == SolidTerminus);

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
