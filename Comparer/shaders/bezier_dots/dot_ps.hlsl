#include "dot_common.hlsli"

float CurveCapSDF(float2 coord, float spanLength, float halfWidth, uint cap)
{
    const float body = coord.x - halfWidth;
    const float overshoot = max(-coord.y, coord.y - spanLength);

    if (cap == CurveCapButt)
    {
        return max(body, overshoot);
    }
    if (cap == CurveCapSquare)
    {
        return max(body, overshoot - halfWidth);
    }
    if (cap == CurveCapTriangleOut)
    {
        return max(body, (coord.x + overshoot - halfWidth) * 0.70710678118);
    }
    if (cap == CurveCapTriangleIn)
    {
        return max(body, (overshoot - coord.x) * 0.70710678118);
    }
    // CurveCapRound
    return length(float2(coord.x, overshoot)) - halfWidth;
}

struct DotVSOutput {
    float4 Position : SV_Position;
    noperspective float4 Color : COLOR0;
    noperspective float2 Local : TEXCOORD0;
    nointerpolation float HalfWidth : TEXCOORD1;
    nointerpolation uint CapCapJoin : TEXCOORD2;
};

float4 main(DotVSOutput input) : SV_Target
{
    const float lateral   = abs(input.Local.x);
    const float along     = input.Local.y;
    const float halfWidth = input.HalfWidth;

    const uint cap = along >= 0.0 ? BackCap(input.CapCapJoin) : FrontCap(input.CapCapJoin);
    const float sdf = CurveCapSDF(float2(lateral, along), 0.0, halfWidth, cap);

    if (sdf > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - sdf));
}
