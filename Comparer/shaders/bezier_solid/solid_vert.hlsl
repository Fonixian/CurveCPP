#include "solid_common.hlsli"

// ONE TRIANGLE PER SEGMENT.
//
// The old geometry was DrawInstanced(5, points - 1) as a TRIANGLESTRIP: v0/v1 at B, v2/v3 at C and
// v4 the join wedge, three triangles whose outline WAS the drawn shape - the rasteriser did the
// bounding and the pixel shader only carved caps out of it. This draws Draw((points - 1) * 3) as a
// TRIANGLELIST instead: one loose triangle that merely CONTAINS the segment, with the bounding moved
// into the pixel shader.
//
// What makes that legal is that SDF.x and SDF.y were never per-vertex constants pretending to be a
// coordinate - they are exactly the affine functions
//     lateral(p) = dot(p - B, lateralDir),   localArc(p) = dot(p - B, segDir)
// evaluated at each vertex (verified to 9e-13 px over 4000 random configurations of the old shader).
// An affine function interpolated over ANY triangle containing a pixel yields the same value there,
// so the pixel shader sees bit-identical inputs no matter what shape delivers them.
//
// What the geometry did contribute, and now has to be said out loud:
//   * where a segment stops at a joint. The old quad ended on the joint's angle bisector (a plain
//     miter) for turns up to 90 deg, which is exactly what ArcShear encodes - see solid_ps.hlsl.
//   * that the strip stopped at |lateral| == halfWidth, clipping the outer half of the coverage
//     ramp. SOLID_AA_MARGIN restores it; set it to 0.0 for the old, half-a-pixel-thin look.

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

StructuredBuffer<float4>          CalculatedPoints : register(t0);
StructuredBuffer<uint>            CurveBegins      : register(t1);
StructuredBuffer<uint>            BezierIndexMap   : register(t4);
StructuredBuffer<SolidCurveStyle> CurveStyles      : register(t6);

bool IsCurveBegin(uint pointIndex) {
    uint packedWord = CurveBegins[pointIndex >> 5u];
    return ((packedWord >> (pointIndex & 31u)) & 1u) != 0u;
}

float4 side_dist(float4 p) { return float4(p.x, -p.x, p.y, -p.y) + p.w; }
float2 depth_dist(float4 p) { return float2(p.z, p.w - p.z); }

float max4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }
float min4(float4 v) { return min(min(v.x, v.y), min(v.z, v.w)); }
float max2(float2 v) { return max(v.x, v.y); }
float min2(float2 v) { return min(v.x, v.y); }

bool clip(inout float4 B, inout float3 color_B,
          inout float4 C, inout float3 color_C,
          inout float4 A, inout float4 D) {
    if (isnan(B.x) || isnan(C.x)) return false;

    float4 sB = side_dist(B), sC = side_dist(C);
    float2 nB = depth_dist(B), nC = depth_dist(C);

    if (any(min(sB, sC) < 0.0) || any(min(nB, nC) < 0.0)) {
        if (any(max(sB, sC) < 0.0) || any(max(nB, nC) < 0.0)) return false;

        float4 ds = sC - sB;
        float4 ts = -sB / ds;
        float4 mix_ts0 = (ds > 0.0) ? ts : 0.0;
        float t0 = max(0.0, max4(mix_ts0));
        float4 mix_ts1 = (ds < 0.0) ? ts : 1.0;
        float t1 = min(1.0, min4(mix_ts1));

        float2 dn = nC - nB;
        float2 tn = -nB / dn;
        float2 mix_tn0 = (dn > 0.0) ? tn : 0.0;
        t0 = max(t0, max2(mix_tn0));
        float2 mix_tn1 = (dn < 0.0) ? tn : 1.0;
        t1 = min(t1, min2(mix_tn1));

        if (t0 >= t1) return false;

        float4 B0 = B, C0 = C;
        B = lerp(B0, C0, t0);
        C = lerp(B0, C0, t1);

        float3 c_B0 = color_B, c_C0 = color_C;
        color_B = lerp(c_B0, c_C0, t0);
        color_C = lerp(c_B0, c_C0, t1);

        float2 nB0 = nB, nC0 = nC;
        nB = lerp(nB0, nC0, t0);
        nC = lerp(nB0, nC0, t1);

        // A clipped end is no longer a real joint: the neighbour is replaced by this segment's own
        // far point, which reads as a fold below and ends the segment flat instead of on a bisector.
        if (t0 > 0.0) A = C;
        if (t1 < 1.0) D = B;
    }

    float2 hA = float2(depth_dist(A).x, A.w);
    float2 eA = float2(nB.x, B.w) - hA;
    float2 tA = -hA / eA;
    float uA = max(max(0.0, eA.x > 0.0 ? tA.x : 0.0), eA.y > 0.0 ? tA.y : 0.0);
    if (uA > 0.0) A = lerp(A, B, uA);

    float2 hD = float2(depth_dist(D).x, D.w);
    float2 eD = float2(nC.x, C.w) - hD;
    float2 tD = -hD / eD;
    float uD = max(max(0.0, eD.x > 0.0 ? tD.x : 0.0), eD.y > 0.0 ? tD.y : 0.0);
    if (uD > 0.0) D = lerp(D, C, uD);
    return true;
}

// --- the bisector frame -------------------------------------------------------------------------
// 1 + cos(turn) below this means the joint is folded back on itself. It is also what a MISSING
// neighbour looks like (NaN) and what a near-plane-clipped end looks like (exactly -segDir). All
// three want the same answer: no bisector here, end the segment flat.
static const float SolidBisectorMinCos = 1e-3;
// Bounds the shear so a near-fold cannot throw the cut line across the screen.
static const float SolidMaxArcShear = 8.0;

float2 SolidSafeDir(float2 from, float2 to) {
    float2 delta = to - from;
    float len = length(delta);
    return (len > 1e-6) ? (delta / len) : float2(0.0, 0.0);
}

// Signed tan(half the turn angle) at one end of the segment, in the pixel shader's lateral frame.
// Splitting a pixel into (localArc along segDir) + (lateral along lateralDir), the arc whose
// iso-lines are the joint's angle bisector is
//     localArc + lateral * dot(lateralDir, t) / dot(segDir, t),   t = segDir + neighbourDir
// and dot(lateralDir, segDir) == 0 collapses the ratio to what is below. The two segments meeting at
// a joint get equal and opposite shears, which describe the SAME line in the plane - that is what
// lets each of them keep its own side of it with no gap and no double blend.
//
// neighbourDir points INTO the joint at B and OUT of it at C, so one function serves both.
// Identical to CurveJointShear in curve_vs.hlsl, plus the isJoint flag. (Duplicated on purpose:
// the solid shaders do not share an hlsli with the patterned ones.)
float SolidJointShear(float2 lateralDir, float2 segDir, float2 neighbourDir, out bool isJoint) {
    float denom = 1.0 + dot(neighbourDir, segDir);
    isJoint = denom > SolidBisectorMinCos;              // false for NaN, which is what we want
    if (!isJoint) return 0.0;
    return clamp(dot(lateralDir, neighbourDir) / denom, -SolidMaxArcShear, SolidMaxArcShear);
}

SolidVSOutput main(uint vertexId : SV_VertexID) {
    SolidVSOutput o = (SolidVSOutput)0;

    const uint i = vertexId / 3u;               // segment = the point pair (i, i + 1)
    const uint corner = vertexId - i * 3u;
    const uint pointCount = TotalPointCount;

    const bool segmentValid = (i + 1 < pointCount) && !IsCurveBegin(i + 1);
    if (!segmentValid) {
        o.Position = 0.0/0.0;
        return o;
    }

    const bool hasA = i > 0 && !IsCurveBegin(i);
    const bool hasD = (i + 2) < pointCount && !IsCurveBegin(i + 2);

    float4 rawB = CalculatedPoints[i];
    float4 rawC = CalculatedPoints[i+1];
    float4 A4 = hasA ? float4(CalculatedPoints[i - 1].xyz, 1.0) : 0.0/0.0;
    float4 B4 = float4(rawB.xyz, 1.0);
    float4 C4 = float4(rawC.xyz, 1.0);
    float4 D4 = hasD ? float4(CalculatedPoints[i + 2].xyz, 1.0) : 0.0/0.0;

    float3 color_B = UnpackColorBits(asuint(rawB.w)).xyz;
    float3 color_C = UnpackColorBits(asuint(rawC.w)).xyz;

    A4 = mul(A4, VP);
    B4 = mul(B4, VP);
    C4 = mul(C4, VP);
    D4 = mul(D4, VP);

    if (!clip(B4, color_B, C4, color_C, A4, D4)) {
        o.Position = 0.0/0.0;
        return o;
    }

    const uint curveIndex = BezierIndexMap[i];
    const SolidCurveStyle style = CurveStyles[curveIndex];
    const float halfWidth = style.Width;

    A4 /= A4.w;
    B4 /= B4.w;
    C4 /= C4.w;
    D4 /= D4.w;

    const float2 pA = mad(A4.xy, float2(0.5, 0.5), float2(0.5, 0.5)) * WH;   // NaN when absent
    const float2 pB = mad(B4.xy, float2(0.5, 0.5), float2(0.5, 0.5)) * WH;
    const float2 pC = mad(C4.xy, float2(0.5, 0.5), float2(0.5, 0.5)) * WH;
    const float2 pD = mad(D4.xy, float2(0.5, 0.5), float2(0.5, 0.5)) * WH;

    const float segLength = distance(pB, pC);
    const float2 segDir = (pC - pB) / segLength;            // NaN on a zero-length segment, which
    const float2 lateralDir = float2(-segDir.y, segDir.x);  // kills the triangle - as it did before

    bool jointB = false, jointC = false;
    float shearB = 0.0, shearC = 0.0;
    if (hasA) shearB = SolidJointShear(lateralDir, segDir, SolidSafeDir(pA, pB), jointB);
    if (hasD) shearC = SolidJointShear(lateralDir, segDir, SolidSafeDir(pC, pD), jointC);

    const uint endB = !hasA ? SolidTerminus : (jointB ? SolidJointBisector : SolidJointFlat);
    const uint endC = !hasD ? SolidTerminus : (jointC ? SolidJointBisector : SolidJointFlat);

    // How far past each end the pixel shader can possibly keep a pixel. At a bisector joint the cut
    // is min(+-lateral * shear, reach) and |lateral| never exceeds halfWidth + the ramp, so the
    // furthest it can reach is that times the shear. Everything here mirrors solid_ps.hlsl exactly;
    // if one of them changes, the other has to.
    const float W = halfWidth + SolidAAMargin;
    const float reach = SolidJoinReach(style.CapCapJoin, halfWidth);
    const float capReach = SolidCapReach(halfWidth);

    float extB = (endB == SolidTerminus) ? capReach
               : ((endB == SolidJointFlat) ? reach : min(W * abs(shearB), reach));
    float extC = (endC == SolidTerminus) ? capReach
               : ((endC == SolidJointFlat) ? reach : min(W * abs(shearC), reach));
    // The pad goes on the ARC extents only. That is enough to lift the triangle's edges clear of the
    // box corners (which they would otherwise touch exactly), and it leaves the lateral extent at
    // exactly halfWidth + SOLID_AA_MARGIN, so setting that margin to 0 really does clip the stroke
    // at halfWidth the way the old strip did.
    extB += SolidTrianglePad;
    extC += SolidTrianglePad;

    // The covering triangle, in (localArc, lateral) pixels. The box the pixel shader can draw in is
    // [-extB, segLength + extC] x [-W, W]; an apex one box-length behind it and a base two
    // box-heights tall at the far end is the minimum-area triangle that contains a box - the box
    // corners at -extB land on the two edges. Twice the box area, but ONE triangle instead of three
    // and three vertices instead of five.
    const float span = segLength + extB + extC;
    const float arc = (corner == 0u) ? (-extB - span) : (segLength + extC);
    const float lat = (corner == 0u) ? 0.0 : ((corner == 1u) ? (2.0 * W) : (-2.0 * W));

    // Position as a pixel offset from B, converted back to NDC the same way everything else here
    // does it. B4.xy is left untouched so the round trip costs no precision.
    const float2 offset = segDir * arc + lateralDir * lat;
    const float t = arc / segLength;

    o.Position.xy = mad(2.0 / WH, offset, B4.xy);
    // The segment's own depth ramp, held inside [B, C] so the apex cannot extrapolate the vertex out
    // through the near or far plane - there is no hardware clipping left to fix that, the w divide
    // already happened.
    o.Position.z = clamp(lerp(B4.z, C4.z, t), min(B4.z, C4.z), max(B4.z, C4.z));
    o.Position.w = 1.0;

    // Unclamped on purpose: the three corner values then lie on the same line as the old strip's
    // per-vertex colours, so the ramp inside the segment is identical (and exactly color_B at
    // localArc 0, which the old stretched-over-the-quad version was only approximately).
    o.Color = float4(lerp(color_B, color_C, t), 1.0);

    o.SDF = float4(lat, arc, halfWidth, segLength);
    o.CapCapJoin = style.CapCapJoin;
    o.Neighbors = uint2(endB, endC);
    o.ArcShear = float2(shearB, shearC);

    return o;
}
