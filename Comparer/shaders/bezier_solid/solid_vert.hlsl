#include "solid_common.hlsli"

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

// t0 used to be CalculatedPoints, written by a solid_calc_points compute pass. That pass is gone: this
// shader evaluates the three samples it needs (B, C and the neighbour) straight from the control points
// at t3, the same slot the patterned and dot vertex shaders read their curve definitions from.
StructuredBuffer<uint>            CurveBegins      : register(t1);
StructuredBuffer<float3>          control_points   : register(t3); // K0,K1,K2,K3 per curve, see LoadCubic
StructuredBuffer<uint>            BezierIndexMap   : register(t4);
StructuredBuffer<ColorData>       colors           : register(t5);
StructuredBuffer<SolidCurveStyle> CurveStyles      : register(t6);
StructuredBuffer<Indices>         indices          : register(t7);

// The cubic's monomial coefficients K0..K3 sit at a fixed stride of four, so where to read them follows
// from the curve index alone - no lookup into `indices` has to come back first. All four loads are
// independent and go out together.
struct Cubic {
    float3 k0, k1, k2, k3;
};

Cubic LoadCubic(uint curveIndex) {
    const uint k = curveIndex << 2u;
    Cubic c;
    c.k0 = control_points[k];
    c.k1 = control_points[k + 1u];
    c.k2 = control_points[k + 2u];
    c.k3 = control_points[k + 3u];
    return c;
}

// A curve's K0 alone - its exact start point, P(0). At a merged joint this is what BOTH segments meeting
// there use for the shared sample: P(1) = K0 + K1 + K2 + K3 of the curve that ends there is only equal
// to it up to a few ulps, and two different spellings of one corner would let the strips crack.
float3 LoadK0(uint curveIndex) {
    return control_points[curveIndex << 2u];
}

// Horner: P(t) = K0 + t * (K1 + t * (K2 + t * K3)) - three fused multiply-adds per component.
float3 EvaluateBezier(Cubic c, float t) {
    return mad(mad(mad(c.k3, t, c.k2), t, c.k1), t, c.k0);
}

// Curve parameter of one sample index - resolution - 1 intervals span t in [0, 1]. `range` is the
// curve's inclusive sample range, Indices.xy.
float SampleT(uint2 range, uint sampleIndex) {
    return float(sampleIndex - range.x) / float(range.y - range.x);
}

// Colour of one sample: world Y clamped to the height band when one is set, otherwise t.
float3 SampleColor(uint4 color, float height, float t) {
    const float minHeight = asfloat(color.z);
    const float maxHeight = asfloat(color.w);
    float blend = t;
    if (minHeight < maxHeight)
        blend = saturate((height - minHeight) / (maxHeight - minHeight));
    return lerp(UnpackColorBits(color.x).rgb, UnpackColorBits(color.y).rgb, blend);
}

bool IsCurveBegin(uint2 words, uint wordBase, uint pointIndex) {
    uint w = ((pointIndex >> 5u) == wordBase) ? words.x : words.y;
    return ((w >> (pointIndex & 31u)) & 1u) != 0u;
}

float4 side_dist(float4 p) { return mad(p.xxyy, float4(1.0, -1.0, 1.0, -1.0), p.wwww); }
float2 depth_dist(float4 p) { return float2(p.z, p.w - p.z); }

float max4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }
float min4(float4 v) { return min(min(v.x, v.y), min(v.z, v.w)); }
float max2(float2 v) { return max(v.x, v.y); }
float min2(float2 v) { return min(v.x, v.y); }

bool clip(inout float4 B, inout float4 C, out float t0, out float t1) {
    t0 = 0.0;
    t1 = 1.0;

    if (isnan(B.x) || isnan(C.x)) return false;

    float4 sB = side_dist(B), sC = side_dist(C);
    float2 nB = depth_dist(B), nC = depth_dist(C);

    if (any(min(sB, sC) < 0.0) || any(min(nB, nC) < 0.0)) {
        if (any(max(sB, sC) < 0.0) || any(max(nB, nC) < 0.0)) return false;

        float4 ds = sC - sB;
        float4 ts = -sB / ds;
        t0 = max(0.0, max4((ds > 0.0) ? ts : 0.0));
        t1 = min(1.0, min4((ds < 0.0) ? ts : 1.0));

        float2 dn = nC - nB;
        float2 tn = -nB / dn;
        t0 = max(t0, max2((dn > 0.0) ? tn : 0.0));
        t1 = min(t1, min2((dn < 0.0) ? tn : 1.0));

        if (t0 >= t1) return false;

        float4 B0 = B, C0 = C;
        B = lerp(B0, C0, t0);
        C = lerp(B0, C0, t1);
    }

    return true;
}

bool calc_overlap(float2 dir_AB, float d, float2 v, float l_AB, float l_CB, float line_width) {
    if (d <= -0.9996) return true;

    float cos_abc = abs(dot(dir_AB, v));
    float sin_abc = rsqrt(max(0.0, 1.0 - cos_abc * cos_abc));
    float l = line_width * cos_abc * sin_abc;
    return l > l_AB || l > l_CB;
}

SolidVSOutput main(uint index : SV_VertexID, uint i : SV_InstanceID) {
    SolidVSOutput o = (SolidVSOutput)0;
    const uint pointCount = TotalPointCount;

    const bool nearSide = index < 2u;
    
    const uint wordBase = i >> 5u;
    const uint2 beginWords = uint2(CurveBegins[wordBase], CurveBegins[wordBase + 1u]);
    const bool hasA = i > 0u && !IsCurveBegin(beginWords, wordBase, i);
    const bool hasD = (i + 2u) < pointCount && !IsCurveBegin(beginWords, wordBase, i + 2u);
    const bool hasN = nearSide ? hasA : hasD;
    
    if ((i + 1u) >= pointCount || IsCurveBegin(beginWords, wordBase, i + 1u)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    // Segment [i, i + 1] belongs to ONE curve, S = the owner of sample i, and S evaluates B and C over
    // its own t in [0, 1]. Only one index-map lookup: across a merged joint the other curve is simply
    // S - 1 or S + 1, because a chain is a run of consecutive curves (see merge_with_previous).
    //
    // The joint rule keeps merged chains watertight: a sample that is the LAST of one curve and the
    // FIRST of the next merged one is always the next curve's K0, never S's P(1) - so the two segments
    // meeting there, and the neighbours that look across it, all see the same bits. Everything below
    // gives exactly the values the old "evaluate every sample with its owning curve" code did; only
    // the lookups changed.
    //
    //   atEnd       C is S's last sample
    //   nextMerged  ... and the chain goes on into S + 1 (i + 2 is in it), so C is S + 1's K0
    //   atStart     B is S's first sample - with hasA, S - 1 is merged into S and holds the neighbour
    const uint curveB = BezierIndexMap[i];
    const Cubic cubicB = LoadCubic(curveB);
    const uint2 rangeB = indices[curveB].first_last;

    const bool atStart = i == rangeB.x;
    const bool atEnd = (i + 1u) == rangeB.y;
    const bool nextMerged = atEnd && hasD;

    const float tB = SampleT(rangeB, i);
    const float tC = SampleT(rangeB, i + 1u);
    const float3 rawB = EvaluateBezier(cubicB, tB);
    //float3 rawC;
    //[branch] if (nextMerged) rawC = LoadK0(curveB + 1u);
    //else rawC = EvaluateBezier(cubicB, tC);
    float3 rawC = EvaluateBezier(cubicB, tC);

    // The neighbour: at most ONE other curve's coefficients, and only on the side that crosses a joint.
    // Its t is that curve's own SampleT - on the previous curve (n - 1) / n, on the next one 1 / n - so
    // it lands on the same bits as the segment on the other side of the joint computes for that sample.
    // When !hasN it is never used (A falls back to C below).
    float3 rawN = 0.0 / 0.0;
    [branch] if (hasN) {
        if (nearSide) {
            // i - 1: in S, or - when B is S's first sample - the previous curve's second-to-last.
            [branch] if (atStart) {
                const uint2 rangeP = indices[curveB - 1u].first_last;
                rawN = EvaluateBezier(LoadCubic(curveB - 1u), SampleT(rangeP, i - 1u));
            }
            else rawN = EvaluateBezier(cubicB, SampleT(rangeB, i - 1u));
        }
        else {
            // i + 2: in S, in S + 1 (when C is S's last), and possibly itself a joint - then it is the
            // following curve's K0, by the same rule as C. "The chain goes on past i + 2" is its begin bit.
            const bool pastD = (i + 3u) < pointCount && !IsCurveBegin(beginWords, wordBase, i + 3u);
            [branch] if (nextMerged) {
                const uint2 rangeNext = indices[curveB + 1u].first_last;
                [branch] if ((i + 2u) == rangeNext.y && pastD) rawN = LoadK0(curveB + 2u);
                else rawN = EvaluateBezier(LoadCubic(curveB + 1u), SampleT(rangeNext, i + 2u));
            }
            else [branch] if ((i + 2u) == rangeB.y && pastD) rawN = LoadK0(curveB + 1u);
            else rawN = EvaluateBezier(cubicB, SampleT(rangeB, i + 2u));
        }
    }

    float4 B4 = mul(float4(rawB, 1.0), VP);
    float4 C4 = mul(float4(rawC, 1.0), VP);
    float4 N4 = mul(float4(rawN, 1.0), VP);

    float t0, t1;
    if (!clip(B4, C4, t0, t1)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    // P is the endpoint this vertex sits on, Q the far one. The neighbour is
    // dropped onto Q when this end was cut off by the frustum.
    const float4 P = nearSide ? B4 : C4;
    const float4 Q = nearSide ? C4 : B4;
    if (nearSide ? (t0 > 0.0) : (t1 < 1.0)) N4 = Q;

    // Pull the neighbour in front of the near plane. Only reachable when the
    // neighbour is actually behind it, so the divides stay off the fast path.
    const float2 hN = N4.zw;
    if (min2(hN) < 0.0) {
        float2 eN = P.zw - hN;
        float2 tN = -hN / eN;
        float uN = max(0.0, max(eN.x > 0.0 ? tN.x : 0.0, eN.y > 0.0 ? tN.y : 0.0));
        if (uN > 0.0) N4 = lerp(N4, P, uN);
    }

    const uint style = CurveStyles[curveB].width_capcapjoin;
    const float width_pixel = StyleWidth(style);

    // C's colour, like its position, is the next curve's at a merged joint: its colours at t = 0.
    const uint4 colorB = colors[curveB].c0_c1_height0_height1;
    uint4 colorC = colorB;
    float tColorC = tC;
    [branch] if (nextMerged) { colorC = colors[curveB + 1u].c0_c1_height0_height1; tColorC = 0.0; }
    const float3 cB = SampleColor(colorB, rawB.y, tB);
    const float3 cC = SampleColor(colorC, rawC.y, tColorC);
    o.Color = float4(lerp(cB, cC, nearSide ? t0 : t1), 1.0);
    o.CapCapJoin = style; // the width byte on top is ignored by FrontCap/BackCap/Join
    o.Neighbors = uint2(hasA ? 1u : 0u, hasD ? 1u : 0u);

    const float3 ndcB = B4.xyz / B4.w;
    const float3 ndcC = C4.xyz / C4.w;
    const float2 ndcN = N4.xy / N4.w;

    o.Position = float4(nearSide ? ndcB : ndcC, 1.0);

    const float2 scrB = mad(ndcB.xy, 0.5, 0.5) * WH;
    const float2 scrC = mad(ndcC.xy, 0.5, 0.5) * WH;
    const float2 scrN = mad(ndcN, 0.5, 0.5) * WH;

    const float2 B = nearSide ? scrB : scrC;
    const float2 C = nearSide ? scrC : scrB;
    const float2 A = (hasN && !isnan(scrN.x)) ? scrN : C;

    const float l_AB = distance(A, B);
    const float l_CB = distance(B, C);
    const float2 dir_AB = (A - B) / l_AB;
    const float2 dir_BC = (B - C) / l_CB;

    const float2 dir_AB_r = float2(dir_AB.y, -dir_AB.x);
    const float2 dir_BC_r = float2(dir_BC.y, -dir_BC.x);

    const float d = dot(dir_AB, dir_BC);
    const float s_12 = (index == 1u || index == 2u) ? -1.0 : 1.0;

    float2 offset;
    if (d >= 0.0) {
        // Dense tessellation keeps consecutive segments near-collinear, so this
        // is the overwhelmingly common case: plain miter, nothing else needed.
        offset = s_12 * ((dir_AB_r + dir_BC_r) / (1.0 + d));
    }
    else {
        const float2 right_offset = (d <= -0.9999) ? dir_AB_r : (dir_AB_r + dir_BC_r) / (1.0 + d);

        // dot(right_offset, inner) only ever carries the sign that built inner,
        // which is this cross product's sign.
        const bool side = dot(dir_AB_r, dir_BC) < 0.0;
        const float2 inner = side ? right_offset : -right_offset;

        // normalize(inner) and the sign flip that follows it cancel: whichever
        // way inner points, the flipped vector is normalize(right_offset).
        // calc_overlap only ever takes abs(dot(dir_AB, v)), so it is unaffected.
        const float2 v = normalize(right_offset);

        if (calc_overlap(dir_AB, d, v, l_AB, l_CB, width_pixel)) {
            offset = dir_BC + s_12 * dir_BC_r;
        }
        else {
            // The a/b swap fires for index 0 and 3 only, and b == a at index 4,
            // so the whole select collapses to this one predicate.
            const bool swap = (index == 0u) || (index == 3u);
            if (index == 4u || (side != swap)) {
                const float cos_half = clamp(dot(dir_BC_r, v), -1.0, 1.0);
                const float t = sqrt(max(0.0, 1.0 - cos_half) / (1.0 + cos_half));
                const bool far4 = index >= 4u;
                const float2 perp_base = far4 ? dir_AB_r : dir_BC_r;
                const float2 parallel = far4 ? -dir_AB : dir_BC;
                offset = (side ? -perp_base : perp_base) + t * parallel;
            }
            else {
                offset = inner;
            }
        }
    }

    float sdf = dot(offset, dir_BC) * -width_pixel;
    if (index >= 2u)
        sdf = l_CB - sdf;

    o.SDF.x = (index == 4u) ? (dot(offset, dir_BC_r) * -width_pixel)
                            : (((index & 1u) == 0u) ? width_pixel : -width_pixel);
    o.SDF.y = sdf;
    o.SDF.zw = float2(width_pixel, l_CB);

    o.Position.xy = mad((2.0 * width_pixel) / WH, offset, o.Position.xy);

    return o;
}
