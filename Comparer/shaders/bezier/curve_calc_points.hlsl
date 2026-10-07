#include "curve_common.hlsli"

// World and screen length of the chord from each sample to the next one of its piece. The sample
// index alone gives the piece (sample >> PieceSampleShift) and t, so the control points are the only
// load. A piece's last sample is the joint duplicate: its chord is 0, so the arc carries straight
// across into the next piece of the chain.

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount; // samples, 64 per piece
    uint     TotalCurveCount; // pieces
};

StructuredBuffer<float3>  ControlPoints  : register(t0);

RWStructuredBuffer<float> WorldDistances  : register(u0);
RWStructuredBuffer<float> ScreenDistances : register(u1);

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint pointIndex = dispatchId.x;
    if (pointIndex >= TotalPointCount) return;

    const uint local = pointIndex & PieceSampleMask;
    if (local == PieceSampleMask) {
        WorldDistances[pointIndex] = 0.0;
        ScreenDistances[pointIndex] = 0.0;
        return;
    }

    const uint k = (pointIndex >> PieceSampleShift) << 2u;
    Cubic cubic;
    cubic.P0 = ControlPoints[k];
    cubic.P1 = ControlPoints[k + 1u];
    cubic.P2 = ControlPoints[k + 2u];
    cubic.P3 = ControlPoints[k + 3u];
    
    float4 p0 = float4(EvaluateBezier(cubic, PieceT(local)), 1.0);
    float4 p1 = float4(EvaluateBezier(cubic, PieceT(local + 1u)), 1.0);
    
    WorldDistances[pointIndex] = distance(p0, p1);
    
    p0 = mul(p0, VP);
    p1 = mul(p1, VP);

    // Only the part in front of the near plane has a screen length.
    if (p0.z < 0.0 && p1.z < 0.0) {
        ScreenDistances[pointIndex] = 0.0;
        return;
    }
    float4 clipped0 = ClipToNearPlane(p0, p1);
    p1 = ClipToNearPlane(p1, p0);
    p0 = clipped0;

    float4 screen = mad(float4(p0.xy / p0.w, p1.xy / p1.w), 0.5, 0.5) * float4(WH, WH);
    ScreenDistances[pointIndex] = distance(screen.xy, screen.zw);
}
