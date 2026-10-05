#include "curve_common.hlsli"

// World and screen chord lengths only. This pass used to also store every sample's position and
// packed colour into CalculatedPoints for curve_vs to build the strip from. curve_vs now evaluates
// B, C and both neighbours from the control points itself (the solid renderer's scheme), so the
// positions are computed here, measured, and dropped - CalculatedPoints is not allocated for this
// renderer any more. The two arc lengths cannot be recomputed on demand (they are prefix sums over
// every preceding sample), so they stay.

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

// The index map hands a merged joint sample to the LATER curve, so its chord is measured along the
// curve that leaves it.
StructuredBuffer<float3>  ControlPoints  : register(t0); // P0..P3 per curve at curveIndex * 4
StructuredBuffer<uint>    BezierIndexMap : register(t1);
StructuredBuffer<Indices> CurveIndices   : register(t2);

RWStructuredBuffer<float> WorldDistances  : register(u0);
RWStructuredBuffer<float> ScreenDistances : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint pointIndex = dispatchId.x;
    if (pointIndex >= TotalPointCount) return;

    const uint curveIndex = BezierIndexMap[pointIndex];
    const uint2 range = CurveIndices[curveIndex].first_last;

    const uint k = curveIndex << 2u;
    Cubic cubic;
    cubic.p0 = ControlPoints[k];
    cubic.p1 = ControlPoints[k + 1u];
    cubic.p2 = ControlPoints[k + 2u];
    cubic.p3 = ControlPoints[k + 3u];

    // The last sample of a curve measures a zero-length chord to itself: t.y is clamped to 1. Through
    // SampleT, so these are the same t values curve_vs evaluates.
    const float2 t = float2(SampleT(range, pointIndex), SampleT(range, min(pointIndex + 1u, range.y)));
    float4 p0 = float4(EvaluateBezier(cubic, t.x), 1.0);
    float4 p1 = float4(EvaluateBezier(cubic, t.y), 1.0);

    // World Distance
    WorldDistances[pointIndex] = distance(p0, p1);

    // Screen Distance
    p0 = mul(p0, VP);
    p1 = mul(p1, VP);

    float4 screen = mad(float4(p0.xy / p0.w, p1.xy / p1.w), 0.5, 0.5) * float4(WH, WH);
    ScreenDistances[pointIndex] = distance(screen.xy, screen.zw);
}
