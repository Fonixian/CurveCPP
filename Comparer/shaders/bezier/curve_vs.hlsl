#include "curve_common.hlsli"

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

// t1 and t3..t7 are solid_vert's slots, so the two evaluate the strip from identical bindings. The
// patterned renderer's own three buffers take what is left: world arc at t0 (CalculatedPoints there
// once - no longer allocated, this shader evaluates the curve itself), screen arc at t2, and the
// pattern offsets at t8, the chain's curve range at t9.
StructuredBuffer<float>        WorldDistances  : register(t0);
StructuredBuffer<uint>         CurveBegins     : register(t1);
StructuredBuffer<float>        ScreenDistances : register(t2);
StructuredBuffer<float3>       ControlPoints   : register(t3); // P0..P3 per curve, see LoadCubic
StructuredBuffer<uint>         BezierIndexMap  : register(t4);
StructuredBuffer<ColorData>    Colors          : register(t5);
StructuredBuffer<PatternStyle> CurveStyles     : register(t6);
StructuredBuffer<Indices>      CurveIndices    : register(t7);
StructuredBuffer<uint>         PatternOffsets  : register(t8);
StructuredBuffer<uint2>        CurveChains     : register(t9); // first, last curve of this curve's chain

// The coefficients sit at a fixed stride of four, so where to read them follows from the curve index
// alone; all four loads are independent and go out together.
Cubic LoadCubic(uint curveIndex) {
    const uint k = curveIndex << 2u;
    Cubic c;
    c.p0 = ControlPoints[k];
    c.p1 = ControlPoints[k + 1u];
    c.p2 = ControlPoints[k + 2u];
    c.p3 = ControlPoints[k + 3u];
    return c;
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

void pull_in_front(inout float4 N, float4 P) {
    const float2 hN = N.zw;
    if (min2(hN) < 0.0) {
        float2 eN = P.zw - hN;
        float2 tN = -hN / eN;
        float uN = max(0.0, max(eN.x > 0.0 ? tN.x : 0.0, eN.y > 0.0 ? tN.y : 0.0));
        if (uN > 0.0) N = lerp(N, P, uN);
    }
}

static const float CurveBisectorMinCos = 1e-3;
static const float CurveMaxArcShear = 8.0;

float2 CurveSafeDir(float2 from, float2 to) {
    float2 delta = to - from;
    float len = length(delta);
    return (len > 1e-6) ? (delta / len) : float2(0.0, 0.0);
}

float CurveJointShear(float2 lateralDir, float2 segDir, float2 neighbourDir) {
    float denom = 1.0 + dot(neighbourDir, segDir);
    if (!(denom > CurveBisectorMinCos)) return 0.0;
    return clamp(dot(lateralDir, neighbourDir) / denom, -CurveMaxArcShear, CurveMaxArcShear);
}

bool calc_overlap(float2 dir_AB, float d, float2 v, float l_AB, float l_CB, float line_width) {
    if (d <= -0.9996) return true;

    float cos_abc = abs(dot(dir_AB, v));
    float sin_abc = rsqrt(max(0.0, 1.0 - cos_abc * cos_abc));
    float l = line_width * cos_abc * sin_abc;
    return l > l_AB || l > l_CB;
}

CurveVSOutput main(uint index : SV_VertexID, uint i : SV_InstanceID) {
    CurveVSOutput o = (CurveVSOutput)0;
    const uint pointCount = TotalPointCount;
    
    const bool nearSide = index < 2u;

    const uint wordBase = i >> 5u;
    const uint2 beginWords = uint2(CurveBegins[wordBase], CurveBegins[wordBase + 1u]);
    const bool hasA = i > 0u && !IsCurveBegin(beginWords, wordBase, i);
    const bool hasD = (i + 2u) < pointCount && !IsCurveBegin(beginWords, wordBase, i + 2u);

    // Bridge instance between two chains (or past the end): nothing to draw, and nothing loaded yet.
    if ((i + 1u) >= pointCount || IsCurveBegin(beginWords, wordBase, i + 1u)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    // Same evaluation as solid_vert.hlsl - see the note there. Segment [i, i + 1] belongs to ONE curve,
    // S = the owner of sample i, which evaluates B and C over its own t in [0, 1] (C = S's P(1) at a
    // merged joint, bit-for-bit the next curve's P0 - see solid_vert).
    const uint curveB = BezierIndexMap[i];
    const Cubic cubicB = LoadCubic(curveB);
    const uint2 rangeB = CurveIndices[curveB].first_last;

    const float tB = SampleT(rangeB, i);
    const float tC = SampleT(rangeB, i + 1u);
    const float3 rawB = EvaluateBezier(cubicB, tB);
    const float3 rawC = EvaluateBezier(cubicB, tC);

    // Unlike solid_vert this needs BOTH neighbours on every vertex, not just the one on its own side:
    // ArcShear is nointerpolation and built from A and D, so all five vertices must agree on it.
    // Each neighbour is evaluated by the curve the adjacent segment uses for that sample, so the two
    // segments at a joint build their miter and shear from the same points: S itself inside S's
    // range (no loads), S - 1 / S + 1 only across a merged joint (a chain is a run of consecutive
    // curves). Without a neighbour the value is never used - A/D fall back to C/B below - so it takes
    // the no-load path, where i - 1 may wrap and only make t meaningless.
    const bool crossesA = hasA && (i == rangeB.x);
    const bool crossesD = hasD && ((i + 1u) == rangeB.y);

    float3 rawA;
    [branch] if (crossesA) {
        const uint curveA = curveB - 1u;
        rawA = EvaluateBezier(LoadCubic(curveA), SampleT(CurveIndices[curveA].first_last, i - 1u));
    }
    else rawA = EvaluateBezier(cubicB, SampleT(rangeB, i - 1u));

    float3 rawD;
    [branch] if (crossesD) {
        const uint curveD = curveB + 1u;
        rawD = EvaluateBezier(LoadCubic(curveD), SampleT(CurveIndices[curveD].first_last, i + 2u));
    }
    else rawD = EvaluateBezier(cubicB, SampleT(rangeB, i + 2u));

    float4 A4 = mul(float4(rawA, 1.0), VP);
    float4 B4 = mul(float4(rawB, 1.0), VP);
    float4 C4 = mul(float4(rawC, 1.0), VP);
    float4 D4 = mul(float4(rawD, 1.0), VP);

    float t0, t1;
    if (!clip(B4, C4, t0, t1)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    // A clipped end is no longer a real joint, and neither is a terminus: the neighbour is replaced
    // by this segment's own far point, which reads as a fold everywhere below and ends the segment
    // flat instead of on a bisector. C4/B4 are post-clip, so this has to win over whatever was
    // evaluated above.
    if (!hasA || t0 > 0.0) A4 = C4;
    if (!hasD || t1 < 1.0) D4 = B4;

    pull_in_front(A4, B4);
    pull_in_front(D4, C4);

    const PatternStyle style = CurveStyles[curveB];
    const float width_pixel = StyleWidth(style.width_capcapjoin);

    const uint4 colorB = Colors[curveB].c0_c1_height0_height1;
    const float3 cB = SampleColor(colorB, rawB.y, tB);
    const float3 cC = SampleColor(colorB, rawC.y, tC); // S's own colours, t = 1 at its end
    const float2 dB = float2(WorldDistances[i], ScreenDistances[i]);
    const float2 dC = float2(WorldDistances[i + 1u], ScreenDistances[i + 1u]);
    const float tEnd = nearSide ? t0 : t1;

    const bool patterned = style.spacing > 0.0;

    o.ScreenArcBegin = lerp(dB.y, dC.y, t0);
    // The terminus test, per segment: an end with no neighbour gets its cap. An end the frustum cut
    // off counts as having one, so it gets the join shape rather than a cap - the old
    // ScreenArcBegin/ScreenArcEnd test behaved that way, because the arc at a clipped end lies
    // strictly inside the chain. (solid_vert sends hasA/hasD alone, so it caps at a clipped chain end.)
    o.Neighbors = uint2((hasA || t0 > 0.0) ? 1u : 0u, (hasD || t1 < 1.0) ? 1u : 0u);
    o.DashLength = style.dash_length;
    // The width byte is dropped (the pixel shader has it in SDF.z) - bit 24 is the patterned flag.
    o.CapCapJoin = (style.width_capcapjoin & 0x00FFFFFFu) | (patterned ? CurvePatternedBit : 0u);

    // --- the pattern coordinate ---------------------------------------------------------------
    // bezier_common.cpp sets one begin bit per CHAIN, so WorldDistances is cumulative along a whole
    // merged chain and restarts at 0 at the next one. Merged curves share their joint sample, so the
    // curves of a chain are contiguous in arc as well as in slots and PatternPosition is one sorted,
    // dense run per chain. World arc maps to slot by an affine, per-curve-constant rule:
    //
    //     slot = floor(arc / spacing) + (sliceBegin - patternBase)
    //
    // Both the divide and the integer bias commute with the lerp below AND with the rasteriser's
    // linear interpolation, so doing them here hands the pixel shader the finished slot coordinate:
    // three interpolants and a per-pixel divide gone, two of three PatternOffsets loads gone.
    //
    // The bias is identically 0 while every curve shares one spacing - the ini counts telescope, so
    // sliceBegin == patternBase. Kept because mixed spacing (a solid curve between two dashed ones)
    // breaks the telescoping and the bias is what absorbs it.
    //
    // The pixel shader may read the chain's own slots and nothing else: [PatternOffsets[first curve],
    // PatternOffsets[last curve + 1]). Chains are runs of consecutive curves and slices are laid out
    // in curve order, so that is one contiguous range. Outside it lies the NEXT or PREVIOUS stroke,
    // whose screen arcs restart at 0 - reading those is how one unmerged curve used to change another.
    float patternCoord = 0.0;
    uint2 patternSlots = uint2(0u, 0u);
    if (patterned) {
        const float curveArcBegin = WorldDistances[rangeB.x];
        const float patternBase = (curveArcBegin > 0.0)
            ? floor(curveArcBegin / style.spacing) + 1.0
            : 0.0;
        patternCoord = lerp(dB.x, dC.x, tEnd) / style.spacing
                     + (float(PatternOffsets[curveB]) - patternBase);

        const uint2 chain = CurveChains[curveB];
        patternSlots = uint2(PatternOffsets[chain.x], PatternOffsets[chain.y + 1u]);
    }
    o.PatternSlots = patternSlots;

    o.ColorPattern = float4(lerp(cB, cC, tEnd), patternCoord);

    const float3 ndcB = B4.xyz / B4.w;
    const float3 ndcC = C4.xyz / C4.w;
    const float2 ndcA = A4.xy / A4.w;
    const float2 ndcD = D4.xy / D4.w;

    const float2 scrA = mad(ndcA, 0.5, 0.5) * WH;
    const float2 scrB = mad(ndcB.xy, 0.5, 0.5) * WH;
    const float2 scrC = mad(ndcC.xy, 0.5, 0.5) * WH;
    const float2 scrD = mad(ndcD, 0.5, 0.5) * WH;

    // Built from the GLOBAL points, never from the side-swapped A/B/C below, so all five vertices
    // agree: ArcShear is nointerpolation and the rasteriser keeps only one vertex's copy.
    {
        const float2 segDir = CurveSafeDir(scrB, scrC);
        const float2 lateralDir = float2(-segDir.y, segDir.x); // the +SDF.x side

        o.ArcShear = float2(
            CurveJointShear(lateralDir, segDir, CurveSafeDir(scrA, scrB)),   // into the joint at B
            CurveJointShear(lateralDir, segDir, CurveSafeDir(scrC, scrD)));  // out of the joint at C
    }

    o.Position = float4(nearSide ? ndcB : ndcC, 1.0);

    // P is the endpoint this vertex sits on, Q the far one, N the neighbour beyond P.
    const float2 B = nearSide ? scrB : scrC;
    const float2 C = nearSide ? scrC : scrB;
    const float2 A = nearSide ? scrA : scrD;

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
        offset = s_12 * ((dir_AB_r + dir_BC_r) / (1.0 + d));
    }
    else {
        const float2 right_offset = (d <= -0.9999) ? dir_AB_r : (dir_AB_r + dir_BC_r) / (1.0 + d);

        // dot(right_offset, inner) only ever carries the sign that built inner, which is this cross
        // product's sign.
        const bool side = dot(dir_AB_r, dir_BC) < 0.0;
        const float2 inner = side ? right_offset : -right_offset;

        // normalize(inner) and the sign flip that follows it cancel: whichever way inner points, the
        // flipped vector is normalize(right_offset). calc_overlap only ever takes abs(dot(dir_AB, v)),
        // so it is unaffected.
        const float2 v = normalize(right_offset);

        if (calc_overlap(dir_AB, d, v, l_AB, l_CB, width_pixel)) {
            offset = dir_BC + s_12 * dir_BC_r;
        }
        else {
            // The a/b swap fires for index 0 and 3 only, and b == a at index 4, so the whole select
            // collapses to this one predicate.
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
