#include "dot_common.hlsli"

#ifndef DOT_QUAD_MARGIN
#define DOT_QUAD_MARGIN 1.5
#endif

// This shader evaluates the curve itself rather than reading positions the point pass stored. A dot
// needs exactly two samples - the pair bracketing it - so fetching them from a buffer of every
// sample on every curve costs more memory than recomputing two cubics costs ALU, and it lets the
// dots renderer skip allocating CalculatedPoints entirely (see dot_calc_points.hlsl).
//
// The tangent still comes from PROJECTING those two samples and subtracting in screen space, not
// from the analytic derivative. Deliberate: a segment's frame everywhere else in these renderers is
// built from the difference of two projected endpoints, and a perspective-correct world-space
// tangent would disagree with it under perspective. Same reasoning as the line body never
// re-projecting an interior point.

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData : register(t3);
StructuredBuffer<DotStyle>        DotStyles  : register(t6);
StructuredBuffer<DotSample>       Dots       : register(t7);

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
    uint curveIndex = s.CurveIndex;

    BezierCurveData bez = BezierData[curveIndex];
    DotStyle style = DotStyles[curveIndex];
    const float halfWidth = Width(style.cap_cap_width);

    float tA = SampleT(bez, s.Sample);
    float tB = SampleT(bez, s.Sample + 1u);

    float3 posA = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, tA);
    float3 posB = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, tB);

    float4 clipA = mul(float4(posA, 1.0), VP);
    float4 clipB = mul(float4(posB, 1.0), VP);

    // Behind the camera: drop the dot rather than reproducing the line body's near-plane clip. An
    // isolated point sprite has no strip continuity to preserve.
    if (clipA.w <= 1e-5 || clipB.w <= 1e-5) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    float2 screenA = ProjectToScreen(clipA);
    float2 screenB = ProjectToScreen(clipB);

    float2 tangent = screenB - screenA;
    float  tangentLen = length(tangent);

    // SampleA == SampleB, which the binary search produces when a dot's target distance lands on or
    // past the curve's last sample. Fall back to the direction of the preceding interval.
    // SampleA > 0 always holds, because Add() asserts resolution >= 2.
    if (tangentLen < 1e-5) {
        uint prevIndex = s.Sample > 0 ? s.Sample - 1 : s.Sample;
        float3 posPrev = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, SampleT(bez, prevIndex));
        float4 clipPrev = mul(float4(posPrev, 1.0), VP);
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

    // The colour used to be interpolated between two per-sample colours the point pass had already
    // packed to 8 bits each. Blending the two RAMP POSITIONS and unpacking once is the same value
    // algebraically - lerp(lerp(C0,C1,a), lerp(C0,C1,b), s) == lerp(C0,C1,lerp(a,b,s)) - and skips
    // the round trip through 8-bit, so the only difference is that it no longer quantises twice.
    float blendA = ColorBlend(bez, posA, tA);
    float blendB = ColorBlend(bez, posB, tB);

    float4 colorBegin = UnpackColorBits(bez.ColorBegin);
    float4 colorEnd   = UnpackColorBits(bez.ColorEnd);
    float3 color = lerp(colorBegin, colorEnd, lerp(blendA, blendB, s.SegmentT)).rgb;

    o.Position = float4(ndcOut, depth, 1.0);
    o.Color = float4(color, 1.0);
    o.Local = cornerSign * extent;
    o.HalfWidth = halfWidth;
    o.CapCapJoin = style.cap_cap_width;

    return o;
}
