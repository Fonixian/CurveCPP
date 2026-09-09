#include "dot_common.hlsli"

#ifndef DOT_QUAD_MARGIN
#define DOT_QUAD_MARGIN 1.5
#endif

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<float4>    CalculatedPoints : register(t0);
StructuredBuffer<uint>      BezierIndexMap   : register(t4);
StructuredBuffer<DotStyle>  DotStyles        : register(t6);
StructuredBuffer<DotSample> Dots             : register(t7);

struct DotVSOutput {
    float4 Position : SV_Position;
    noperspective float4 Color : COLOR0;
    noperspective float2 Local : TEXCOORD0;
    nointerpolation float HalfWidth : TEXCOORD1;
    nointerpolation uint CapCapJoin : TEXCOORD2;
};

float2 ProjectToScreen(float4 clipPos) {
    float2 ndc = clipPos.xy / clipPos.w;
    return mad(ndc, float2(0.5, 0.5), float2(0.5, 0.5)) * WH;
}

DotVSOutput main(uint vertexId : SV_VertexID, uint dotId : SV_InstanceID) {
    DotVSOutput o = (DotVSOutput)0;

    DotSample s = Dots[dotId];
    uint curveIndex = BezierIndexMap[s.SampleA];
    DotStyle style = DotStyles[curveIndex];
    const float halfWidth = style.Width;

    float4 rawA = CalculatedPoints[s.SampleA];
    float4 rawB = CalculatedPoints[s.SampleB];

    float4 clipA = mul(float4(rawA.xyz, 1.0), VP);
    float4 clipB = mul(float4(rawB.xyz, 1.0), VP);

    if (clipA.w <= 1e-5 || clipB.w <= 1e-5) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    float2 screenA = ProjectToScreen(clipA);
    float2 screenB = ProjectToScreen(clipB);

    float2 tangent = screenB - screenA;
    float  tangentLen = length(tangent);

    if (tangentLen < 1e-5) {
        uint prevIndex = s.SampleA > 0 ? s.SampleA - 1 : s.SampleA;
        float4 clipPrev = mul(float4(CalculatedPoints[prevIndex].xyz, 1.0), VP);
        if (clipPrev.w > 1e-5) {
            float2 screenPrev = ProjectToScreen(clipPrev);
            tangent = screenA - screenPrev;
            tangentLen = length(tangent);
        }
    }
    tangent = (tangentLen > 1e-5) ? (tangent / tangentLen) : float2(1.0, 0.0);

    float2 lateralDir = float2(-tangent.y, tangent.x);
    float2 screenPos = lerp(screenA, screenB, s.SegmentT);
    float  depth = lerp(clipA.z / clipA.w, clipB.z / clipB.w, s.SegmentT);

    const float extent = halfWidth + DOT_QUAD_MARGIN;

    float2 cornerSign;
    if (vertexId == 0)      cornerSign = float2(-1.0, -1.0);
    else if (vertexId == 1) cornerSign = float2( 1.0, -1.0);
    else if (vertexId == 2) cornerSign = float2(-1.0,  1.0);
    else                    cornerSign = float2( 1.0,  1.0);

    float2 offsetPx = cornerSign.x * extent * lateralDir + cornerSign.y * extent * tangent;
    float2 finalScreen = screenPos + offsetPx;
    float2 ndcOut = finalScreen / WH * 2.0 - 1.0;

    float3 colorA = UnpackColorBits(asuint(rawA.w)).rgb;
    float3 colorB = UnpackColorBits(asuint(rawB.w)).rgb;

    o.Position = float4(ndcOut, depth, 1.0);
    o.Color = float4(lerp(colorA, colorB, s.SegmentT), 1.0);
    o.Local = cornerSign * extent;
    o.HalfWidth = halfWidth;
    o.CapCapJoin = style.CapCapJoin;

    return o;
}
