// One thread per sample: evaluates the sample ONCE and stores it, so solid_vert reads positions
// instead of re-evaluating the curve for every vertex of every segment (about 15 evaluations per
// sample in the vertex shader, one here).
//
// Positions are world space, so the pass does not depend on the camera: BezierSolidRenderer reruns it
// only after the control points or the sample layout changed.
//
// x, y, z: world position of the sample, evaluated by the curve that OWNS it (BezierIndexMap). At a
//          merged joint that is the later curve, at its t = 0 - the one position every segment
//          touching the joint then agrees on.
// w:       the sample's t on its owning curve, [0, 1]. The expression is the same SampleT the vertex
//          shader used, so it is bit-identical. solid_vert recovers the end t of a segment that ends on
//          a joint from it (the next sample is then another curve's t = 0, which is not larger).

// Cubic, EvaluateBezier and SampleT are the ones solid_vert used to carry; the solid shaders keep their
// own copies on purpose (no shared hlsli with the patterned renderer), and this is now their only home.

struct Cubic {
    float3 P0, P1, P2, P3;
};

float3 EvaluateBezier(Cubic c, float t) {
    // Bernstein form
    float s   = 1.0 - t;
    float st3 = 3.0 * s * t;
    return mad(c.P3, t * t * t, mad(c.P2, st3 * t, mad(c.P1, st3 * s, c.P0 * (s * s * s))));
}

// [range.x, range.y] -> [0.0, 1.0]
float SampleT(uint2 range, uint sampleIndex) {
    return float(sampleIndex - range.x) / float(range.y - range.x);
}

cbuffer CameraData : register(b0) {
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<float3> CurveControlPoints : register(t0); // P0..P3 per curve, curveIndex * 4
StructuredBuffer<uint>   BezierIndexMap     : register(t1); // owning curve per sample
StructuredBuffer<uint2>  CurveIndices       : register(t2); // FirstIndex, LastIndex per curve

RWStructuredBuffer<float4> SamplePositions : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint sampleIndex = dispatchId.x;
    if (sampleIndex >= TotalPointCount) return;

    uint curve = BezierIndexMap[sampleIndex];
    uint2 range = CurveIndices[curve];

    uint k = curve << 2u;
    Cubic c;
    c.P0 = CurveControlPoints[k];
    c.P1 = CurveControlPoints[k + 1u];
    c.P2 = CurveControlPoints[k + 2u];
    c.P3 = CurveControlPoints[k + 3u];

    float t = SampleT(range, sampleIndex);
    SamplePositions[sampleIndex] = float4(EvaluateBezier(c, t), t);
}
