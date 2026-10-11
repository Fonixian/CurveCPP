#include "dot_common.hlsli"

// World segment lengths only.

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<float3>  ControlPoints  : register(t0);
StructuredBuffer<uint>    BezierIndexMap : register(t1);
StructuredBuffer<uint2>   CurveIndices   : register(t2);

RWStructuredBuffer<float> WorldDistances : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint pointIndex = dispatchId.x;
    if (pointIndex >= TotalPointCount) return;

    uint curveIndex = BezierIndexMap[pointIndex];
    uint2 range = CurveIndices[curveIndex];

    uint k = curveIndex << 2u;
    Cubic cubic;
    cubic.P0 = ControlPoints[k];
    cubic.P1 = ControlPoints[k + 1u];
    cubic.P2 = ControlPoints[k + 2u];
    cubic.P3 = ControlPoints[k + 3u];

    float2 t = float2(SampleT(range, pointIndex), SampleT(range, min(pointIndex + 1u, range.y)));
    WorldDistances[pointIndex] = distance(EvaluateBezier(cubic, t.x), EvaluateBezier(cubic, t.y));
}
