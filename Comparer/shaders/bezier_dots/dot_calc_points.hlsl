#include "dot_common.hlsli"

// World segment lengths only, into the high parts of the compensated pairs the scan works on (lo = 0).

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<Cubic>   ControlPoints  : register(t0); // one 3x4 per curve, rows x/y/z, columns P0..P3
StructuredBuffer<uint>    BezierIndexMap : register(t1); // curve | segment flags per sample
StructuredBuffer<uint2>   CurveIndices   : register(t2);

RWStructuredBuffer<float2> WorldDistances : register(u0); // (hi, lo)

Cubic LoadCubic(uint curveIndex) {
    return ControlPoints[curveIndex];
}

// One thread measures TWO consecutive points: i0 = 2 * id and i1 = i0 + 1. Point i's value is the
// chord from sample i to sample i + 1 (the next index clamped to the curve's range), so the pair needs
// the positions at i0, i0 + 1 and i0 + 2. When i1 sits on the same curve as i0 the middle one is
// shared - 3 curve evaluations instead of 4, and one curve lookup instead of two. Dispatch
// GroupCount(TotalPointCount, 512).
[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint i0 = dispatchId.x << 1u;
    if (i0 >= TotalPointCount) return;
    uint i1 = i0 + 1u;

    uint curve0 = BezierIndexMap[i0] & SegmentCurveMask;
    uint2 range0 = CurveIndices[curve0];
    Cubic cubic0 = LoadCubic(curve0);

    float3 pos0 = EvaluateBezier(cubic0, SampleT(range0, i0));
    float3 pos1 = EvaluateBezier(cubic0, SampleT(range0, min(i1, range0.y)));
    WorldDistances[i0] = float2(distance(pos0, pos1), 0.0);

    if (i1 >= TotalPointCount) return;

    uint curve1 = BezierIndexMap[i1] & SegmentCurveMask;
    if (curve1 == curve0 && i1 <= range0.y) {
        // Same curve and i1 is not clamped, so its start position is pos1 exactly.
        float3 pos2 = EvaluateBezier(cubic0, SampleT(range0, min(i1 + 1u, range0.y)));
        WorldDistances[i1] = float2(distance(pos1, pos2), 0.0);
    } else {
        // The pair straddles a curve boundary: i1 starts its own curve.
        uint2 range1 = CurveIndices[curve1];
        Cubic cubic1 = LoadCubic(curve1);
        float2 t = float2(SampleT(range1, i1), SampleT(range1, min(i1 + 1u, range1.y)));
        WorldDistances[i1] = float2(distance(EvaluateBezier(cubic1, t.x), EvaluateBezier(cubic1, t.y)), 0.0);
    }
}
