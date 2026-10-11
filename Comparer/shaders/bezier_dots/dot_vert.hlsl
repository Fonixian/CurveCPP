#include "dot_common.hlsli"

#ifndef DOT_QUAD_MARGIN
#define DOT_QUAD_MARGIN 1.5
#endif

// Drawn as DrawInstancedIndirect(4, dot count) with a TRIANGLESTRIP, one quad per dot:
//
//                 2 ------- 3
//                 |    ^    |
//                 |    *    |    * : the dot's center, between sample and sample + 1
//                 |         |    ^ : the screen tangent, towards the curve's end
//                 0 ------- 1
//
// The quad reaches halfWidth + DOT_QUAD_MARGIN from the center, so the antialiased edge fits.
// The two samples around the dot are evaluated here rather than read back from a point buffer, and
// the tangent is taken between their projections, not from the curve's derivative, so it matches
// the frame the stroke renderers build their segments in.

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

StructuredBuffer<float3>    CurveControlPoints : register(t3);
StructuredBuffer<DotSample> Dots               : register(t4); // where solid_vert has the index map
StructuredBuffer<uint4>     CurveColors        : register(t5);
StructuredBuffer<DotStyle>  CurveStyles        : register(t6);
StructuredBuffer<uint2>     CurveIndices       : register(t7);

Cubic LoadCubic(uint curveIndex) {
    uint k = curveIndex << 2u;
    Cubic c;
    c.P0 = CurveControlPoints[k];
    c.P1 = CurveControlPoints[k + 1u];
    c.P2 = CurveControlPoints[k + 2u];
    c.P3 = CurveControlPoints[k + 3u];
    return c;
}

float4 CalculateColor(uint4 colorData, float2 height, float2 t, float segmentT) {
    float minHeight = asfloat(colorData.z);
    float maxHeight = asfloat(colorData.w);
    float3 colorA = UnpackColorBits(colorData.x).rgb;
    float3 colorB = UnpackColorBits(colorData.y).rgb;
    float2 blendAtEnds = (minHeight < maxHeight) ? saturate((height - minHeight) / (maxHeight - minHeight)) : t;
    float blend = lerp(blendAtEnds.x, blendAtEnds.y, segmentT);
    return float4(lerp(colorA, colorB, blend), 1.0);
}

DotVSOutput main(uint vertexId : SV_VertexID, uint dotIndex : SV_InstanceID) {
    DotVSOutput o = (DotVSOutput)0;

    DotSample dotSample = Dots[dotIndex];
    uint curveIndex = dotSample.CurveIndex;

    Cubic cubic = LoadCubic(curveIndex);
    uint2 range = CurveIndices[curveIndex];

    float tStart = SampleT(range, dotSample.Sample);
    float tEnd = SampleT(range, dotSample.Sample + 1u);
    float3 startWorld = EvaluateBezier(cubic, tStart);
    float3 endWorld = EvaluateBezier(cubic, tEnd);

    float4 startClip = mul(float4(startWorld, 1.0), VP);
    float4 endClip = mul(float4(endWorld, 1.0), VP);

    // Behind the camera: drop the dot. Unlike a strip there is no continuity to keep, so no clipping.
    if (startClip.w <= 1e-5 || endClip.w <= 1e-5) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    float3 startNdc = startClip.xyz / startClip.w;
    float3 endNdc = endClip.xyz / endClip.w;

    float2 startPx = mad(startNdc.xy, 0.5, 0.5) * WH;
    float2 endPx = mad(endNdc.xy, 0.5, 0.5) * WH;

    float2 dir = endPx - startPx;
    float dirLength = length(dir);

    // Both samples land on the same pixel (seen end-on): take the direction from the sample before.
    if (dirLength < 1e-5) {
        uint prevSample = dotSample.Sample > 0 ? dotSample.Sample - 1 : dotSample.Sample;
        float4 prevClip = mul(float4(EvaluateBezier(cubic, SampleT(range, prevSample)), 1.0), VP);
        if (prevClip.w > 1e-5) {
            float2 prevPx = mad(prevClip.xy / prevClip.w, 0.5, 0.5) * WH;
            dir = startPx - prevPx;
            dirLength = length(dir);
        }
    }
    dir = (dirLength > 1e-5) ? (dir / dirLength) : float2(1.0, 0.0);
    float2 right = float2(dir.y, -dir.x);

    uint style = CurveStyles[curveIndex].width_capcap;
    float halfWidth = HalfWidth(style);
    float extent = halfWidth + DOT_QUAD_MARGIN;

    o.Color = CalculateColor(CurveColors[curveIndex], float2(startWorld.y, endWorld.y), float2(tStart, tEnd), dotSample.SegmentT);
    o.CapCapJoin = style; // FrontCap/BackCap ignore the width byte

    float2 local = extent * float2((vertexId & 1u) ? 1.0 : -1.0, (vertexId & 2u) ? 1.0 : -1.0);
    o.SDF = float3(local, halfWidth);

    o.Position = float4(lerp(startNdc, endNdc, dotSample.SegmentT), 1.0);
    o.Position.xy = mad(2.0 / WH, local.x * right + local.y * dir, o.Position.xy);

    return o;
}
