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

// The index map is the one per-sample buffer this renderer still reads, and only here: at a merged
// joint it hands the shared sample to the LATER curve, so that sample's chord is measured along the
// curve that leaves it. Nothing after this pass needs it - dot_calc and dot_vert stay inside one curve.
StructuredBuffer<float3>  ControlPoints  : register(t0); // K0..K3 per curve at curveIndex * 4
StructuredBuffer<uint>    BezierIndexMap : register(t1);
StructuredBuffer<Indices> CurveIndices   : register(t2);

RWStructuredBuffer<float> Distances : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint pointIndex = dispatchId.x;
    if (pointIndex >= TotalPointCount) return;

    uint curveIndex = BezierIndexMap[pointIndex];
    uint2 range = CurveIndices[curveIndex].first_last;

    uint firstIndex = range.x;
    uint lastIndex  = range.y;
    uint resolution = lastIndex - firstIndex; // resolution - 1

    // One float per point. This used to be a float2 with .y pinned at 0, because SegmentedScan's
    // element type was fixed at float2 for the patterned renderer's sake; that renderer splits its
    // two channels into separate buffers now and the scan is scalar, so the dead channel is gone.
    float dist = 0.0;
    if (pointIndex < lastIndex) {
        const uint k = curveIndex << 2u;
        Cubic cubic;
        cubic.k0 = ControlPoints[k];
        cubic.k1 = ControlPoints[k + 1u];
        cubic.k2 = ControlPoints[k + 2u];
        cubic.k3 = ControlPoints[k + 3u];

        float t     = float(pointIndex - firstIndex) / float(resolution);
        float tNext = float(pointIndex - firstIndex + 1) / float(resolution);

        float3 position     = EvaluateBezier(cubic, t);
        float3 nextPosition = EvaluateBezier(cubic, tNext);
        dist = distance(position, nextPosition);
    }

    Distances[pointIndex] = dist;
}
