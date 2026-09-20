#include "dot_common.hlsli"

// Chord lengths only. This pass used to also store every sample's world position and packed colour
// into CalculatedPoints, the way the patterned and solid renderers do, because their vertex shaders
// build a strip out of those samples and genuinely need them all. A dot renderer does not: it only
// ever touches the two samples bracketing each dot, and dot_vert can evaluate those two itself from
// the same control points for a handful of ALU. So the positions are computed here, measured, and
// dropped - CalculatedPoints is not allocated for this renderer at all, which takes 16 bytes per
// sample point of storage and a float4 store per point out of the frame.
//
// What survives is Distances: one chord length per point, which the segmented scan turns into
// cumulative arc length. That cannot be recomputed on demand - arc length is a prefix sum over every
// preceding sample, not a function of one t - so it stays.

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData     : register(t0);
StructuredBuffer<uint>            BezierIndexMap : register(t1);

RWStructuredBuffer<float> Distances : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint pointIndex = dispatchId.x;
    if (pointIndex >= TotalPointCount) return;

    uint curveIndex = BezierIndexMap[pointIndex];
    BezierCurveData bez = BezierData[curveIndex];

    uint firstIndex = (uint)bez.FirstIndex;
    uint lastIndex  = (uint)bez.LastIndex;
    uint resolution = lastIndex - firstIndex; // resolution - 1

    // One float per point. This used to be a float2 with .y pinned at 0, because SegmentedScan's
    // element type was fixed at float2 for the patterned renderer's sake; that renderer splits its
    // two channels into separate buffers now and the scan is scalar, so the dead channel is gone.
    float dist = 0.0;
    if (pointIndex < lastIndex) {
        float t     = float(pointIndex - firstIndex) / float(resolution);
        float tNext = float(pointIndex - firstIndex + 1) / float(resolution);

        float3 position     = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, t);
        float3 nextPosition = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, tNext);
        dist = distance(position, nextPosition);
    }

    Distances[pointIndex] = dist;
}
