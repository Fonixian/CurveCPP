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
StructuredBuffer<float3>          control_points   : register(t3); // P0..P3 per curve, see LoadCubic
StructuredBuffer<uint>            BezierIndexMap   : register(t4);
StructuredBuffer<ColorData>       colors           : register(t5);
StructuredBuffer<SolidCurveStyle> CurveStyles      : register(t6);
StructuredBuffer<Indices>         indices          : register(t7);

// The cubic's control points P0..P3 sit at a fixed stride of four, so where to read them follows
// from the curve index alone - no lookup into `indices` has to come back first. All four loads are
// independent and go out together.
struct Cubic {
    float3 p0, p1, p2, p3;
};

Cubic LoadCubic(uint curveIndex) {
    const uint k = curveIndex << 2u;
    Cubic c;
    c.p0 = control_points[k];
    c.p1 = control_points[k + 1u];
    c.p2 = control_points[k + 2u];
    c.p3 = control_points[k + 3u];
    return c;
}

// Bernstein form, straight over the control points:
//
//     P(t) = s^3 P0 + 3 s^2 t P1 + 3 s t^2 P2 + t^3 P3,    s = 1 - t
//
// The weights come out as exactly (1, 0, 0, 0) at t = 0 and (0, 0, 0, 1) at t = 1, so the curve
// reaches P0 and P3 bit-exactly, in whatever order the compiler sums the terms. That is what makes a
// merged joint exact: the curve ending there evaluates P(1) = its P3, the one starting there P(0) = its
// P0, and those are the same control point. (The monomial form K0 + K1 + K2 + K3 only got within a few
// ulps of P3.) Cost: ~9 scalar ops for the weights plus a mul and three mads per component, against
// Horner's three mads per component; the loads are the same four float3.
float3 EvaluateBezier(Cubic c, float t) {
    const float s   = 1.0 - t;
    const float st3 = 3.0 * s * t;
    return mad(c.p3, t * t * t, mad(c.p2, st3 * t, mad(c.p1, st3 * s, c.p0 * (s * s * s))));
}

// Curve parameter of one sample index - resolution - 1 intervals span t in [0, 1]. `range` is the
// curve's inclusive sample range, Indices.first_last. The last sample is pinned to exactly 1: a GPU
// divide may be a reciprocal and a multiply, and n * (1 / n) is not always 1 in float (n = 41 is the
// first), which would leave P(1) a hair short of P3 and undo the exact endpoint EvaluateBezier gives.
float SampleT(uint2 range, uint sampleIndex) {
    return sampleIndex == range.y ? 1.0 : float(sampleIndex - range.x) / float(range.y - range.x);
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
    // its own t in [0, 1] - including C = P(1) on S's last segment. At a merged joint that is S's P3,
    // bit-for-bit the next curve's P0 (Bernstein weights are exactly 0/1 at the ends and SampleT pins
    // the last sample to t = 1), so the two segments meeting there share their corner exactly.
    //
    // The neighbour IS kept consistent: it is always evaluated by the curve the adjacent segment uses
    // for that same sample, so both segments build their miter from the same three points. Inside S's
    // range that is S itself - no loads. Only across a merged joint is it another curve, and then it is
    // simply S - 1 or S + 1, because a chain is a run of consecutive curves (see merge_with_previous).
    const uint curveB = BezierIndexMap[i];
    const Cubic cubicB = LoadCubic(curveB);
    const uint2 rangeB = indices[curveB].first_last;

    const float tB = SampleT(rangeB, i);
    const float tC = SampleT(rangeB, i + 1u);
    const float3 rawB = EvaluateBezier(cubicB, tB);
    const float3 rawC = EvaluateBezier(cubicB, tC);

    // i - 1 leaves S only when B is S's first sample; i + 2 only when C is S's last. With hasN that
    // means the chain goes on into S - 1 / S + 1. When !hasN the value is never used (A falls back to C
    // below), so it takes the no-load path too - ni may wrap there, which only makes t meaningless.
    const uint ni = nearSide ? (i - 1u) : (i + 2u);
    const bool crossesJoint = hasN && (nearSide ? (i == rangeB.x) : ((i + 1u) == rangeB.y));
    float3 rawN;
    [branch] if (crossesJoint) {
        const uint curveN = nearSide ? (curveB - 1u) : (curveB + 1u);
        rawN = EvaluateBezier(LoadCubic(curveN), SampleT(indices[curveN].first_last, ni));
    }
    else rawN = EvaluateBezier(cubicB, SampleT(rangeB, ni));

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

    const uint4 colorB = colors[curveB].c0_c1_height0_height1;
    const float3 cB = SampleColor(colorB, rawB.y, tB);
    const float3 cC = SampleColor(colorB, rawC.y, tC); // S's own colours, t = 1 at its end
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
