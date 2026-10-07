#include "curve_common.hlsli"

// World and screen segment lengths only.

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<float3>  ControlPoints  : register(t0);
StructuredBuffer<uint>    BezierIndexMap : register(t1);
StructuredBuffer<uint2>   CurveIndices   : register(t2);

RWStructuredBuffer<float> WorldDistances  : register(u0);
RWStructuredBuffer<float> ScreenDistances : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint pointIndex = dispatchId.x;
    if (pointIndex >= TotalPointCount) return;

    const uint curveIndex = BezierIndexMap[pointIndex];
    const uint2 range = CurveIndices[curveIndex];

    const uint k = curveIndex << 2u;
    Cubic cubic;
    cubic.P0 = ControlPoints[k];
    cubic.P1 = ControlPoints[k + 1u];
    cubic.P2 = ControlPoints[k + 2u];
    cubic.P3 = ControlPoints[k + 3u];
    
    const float2 t = float2(SampleT(range, pointIndex), SampleT(range, min(pointIndex + 1u, range.y)));
    float4 p0 = float4(EvaluateBezier(cubic, t.x), 1.0);
    float4 p1 = float4(EvaluateBezier(cubic, t.y), 1.0);
    
    WorldDistances[pointIndex] = distance(p0, p1);
    
    p0 = mul(p0, VP);
    p1 = mul(p1, VP);

    float4 screen = mad(float4(p0.xy / p0.w, p1.xy / p1.w), 0.5, 0.5) * float4(WH, WH);
    ScreenDistances[pointIndex] = distance(screen.xy, screen.zw);
}
