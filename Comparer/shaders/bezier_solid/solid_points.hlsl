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

// One row per coordinate, one column per control point: row x = (P0.x, P1.x, P2.x, P3.x), likewise y
// and z, so P3 sits in the .w of every row. row_major so the rows sit in memory in that order, matching
// the CPU's XMFLOAT3X4 whatever the compiler's default matrix packing is. 48 B, three 16-byte loads.
struct Cubic {
    row_major float3x4 P;
};

float3 EvaluateBezier(Cubic c, float t) {
    // Bernstein form
    float s   = 1.0 - t;
    float st3 = 3.0 * s * t;
    return mul(c.P, float4(s * s * s, st3 * s, st3 * t, t * t * t));
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

StructuredBuffer<Cubic>  CurveControlPoints : register(t0); // one 3x4 per curve, rows x/y/z, columns P0..P3
StructuredBuffer<uint>   BezierIndexMap     : register(t1); // owning curve per sample
StructuredBuffer<uint2>  CurveIndices       : register(t2); // FirstIndex, LastIndex per curve

RWStructuredBuffer<float4> SamplePositions : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint sampleIndex = dispatchId.x;
    if (sampleIndex >= TotalPointCount) return;

    uint curve = BezierIndexMap[sampleIndex];
    uint2 range = CurveIndices[curve];

    Cubic c = CurveControlPoints[curve];

    float t = SampleT(range, sampleIndex);
    SamplePositions[sampleIndex] = float4(EvaluateBezier(c, t), t);
}
