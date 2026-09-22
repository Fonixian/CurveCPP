#include "curve_common.hlsli"

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

StructuredBuffer<float4> CalculatedPoints : register(t0);
StructuredBuffer<uint> CurveBegins : register(t1);
StructuredBuffer<float> WorldDistances : register(t2);
StructuredBuffer<BezierCurveData> BezierData : register(t3);
StructuredBuffer<uint> BezierIndexMap : register(t4);
StructuredBuffer<uint> PatternOffsets : register(t5);
StructuredBuffer<CurveStyle> CurveStyles : register(t6);
StructuredBuffer<float> ScreenDistances : register(t7);

bool IsCurveBegin(uint2 words, uint wordBase, uint pointIndex) {
    uint w = ((pointIndex >> 5u) == wordBase) ? words.x : words.y;
    return ((w >> (pointIndex & 31u)) & 1u) != 0u;
}

uint step_over_duplicate(uint first, uint second, bool hasSecond, float3 anchor) {
    return (hasSecond && distance(CalculatedPoints[first].xyz, anchor) < 0.00001) ? second : first;
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

    // Vertices 0,1 sit on B, vertices 2,3,4 on C (4 is the join wedge). Which end a vertex belongs
    // to decides which corner it miters, which colour and arc length it carries, and which of the
    // two clip parameters applies to it.
    const bool nearSide = index < 2u;

    const uint wordBase = (i > 0u ? i - 1u : 0u) >> 5u;
    const uint2 beginWords = uint2(CurveBegins[wordBase], CurveBegins[wordBase + 1u]);
    const bool hasA = i > 0u && !IsCurveBegin(beginWords, wordBase, i);
    const bool hasD = (i + 2u) < pointCount && !IsCurveBegin(beginWords, wordBase, i + 2u);

    // Every memory request is issued before the first branch so the two-step
    // BezierIndexMap -> CurveStyles chain overlaps the transform + clip math. Unlike the solid
    // shader this one cannot drop the far neighbour: ArcShear is nointerpolation and has to hold the
    // same pair of shears whichever vertex the rasteriser happens to take it from, so both ends of
    // the joint frame are needed on every vertex.
    //
    // A missing neighbour is indexed onto this segment's own far endpoint rather than carried as a
    // NaN sentinel: every index stays in range (i - 1u wraps to 0xFFFFFFFF at i == 0), and the value
    // is overwritten by the terminus branch below anyway.
    const float4 rawB = CalculatedPoints[i];
    const float4 rawC = CalculatedPoints[i + 1u];

    const uint iA = hasA ? step_over_duplicate(i - 1u, i - 2u,
                               i > 1u && !IsCurveBegin(beginWords, wordBase, i - 1u), rawB.xyz)
                         : (i + 1u);
    const uint iD = hasD ? step_over_duplicate(i + 2u, i + 3u,
                               i + 3u < pointCount && !IsCurveBegin(beginWords, wordBase, i + 3u), rawC.xyz)
                         : i;

    const float3 rawA = CalculatedPoints[iA].xyz;
    const float3 rawD = CalculatedPoints[iD].xyz;

    if ((i + 1u) >= pointCount || IsCurveBegin(beginWords, wordBase, i + 1u)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    float4 A4 = mul(float4(rawA, 1.0), VP);
    float4 B4 = mul(float4(rawB.xyz, 1.0), VP);
    float4 C4 = mul(float4(rawC.xyz, 1.0), VP);
    float4 D4 = mul(float4(rawD, 1.0), VP);

    float t0, t1;
    if (!clip(B4, C4, t0, t1)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    // A clipped end is no longer a real joint, and neither is a terminus: the neighbour is replaced
    // by this segment's own far point, which reads as a fold everywhere below and ends the segment
    // flat instead of on a bisector. C4/B4 are post-clip, so this has to win over whatever iA/iD
    // indexed.
    if (!hasA || t0 > 0.0) A4 = C4;
    if (!hasD || t1 < 1.0) D4 = B4;

    pull_in_front(A4, B4);
    pull_in_front(D4, C4);

    const uint curveIndex = BezierIndexMap[i];
    const CurveStyle style = CurveStyles[curveIndex];
    const float width_pixel = style.Width;

    const float3 cB = UnpackColorBits(asuint(rawB.w)).xyz;
    const float3 cC = UnpackColorBits(asuint(rawC.w)).xyz;
    const float2 dB = float2(WorldDistances[i], ScreenDistances[i]);
    const float2 dC = float2(WorldDistances[i + 1u], ScreenDistances[i + 1u]);
    const float tEnd = nearSide ? t0 : t1;

    o.Color = float4(lerp(cB, cC, tEnd), 1.0);
    o.TotalDistance = lerp(dB.x, dC.x, tEnd);
    o.ScreenArcBegin = lerp(dB.y, dC.y, t0);
    o.ScreenArcEnd = ScreenDistances[BezierData[curveIndex].LastIndex];
    o.Spacing = style.Spacing;
    o.DashLength = style.DashLength;
    o.CapCapJoin = style.CapCapJoin;

    // --- where this curve's centres live in the flat pattern array ----------------------------
    // WorldDistances is chain-cumulative, so the centre grid is shared by every curve in the chain and
    // curve_pattern_ini hands each curve the WINDOW of it that falls inside its own arc span. The
    // pixel shader inverts a pixel's distance back into a GLOBAL grid index, so it needs the bias
    // that maps that index onto this curve's slice - patternFirst - patternBase.
    //
    // It also needs to be allowed one slot past either end of the slice: a dash whose centre sits
    // on the next curve can still reach back across the joint, and without the widening it would be
    // sliced off exactly at the joint - the very seam this is meant to remove. The flat array is an
    // exclusive scan over the counts in curve order, and the chain runs in curve order too, so
    // sliceBegin - 1 IS the previous centre along the chain and sliceEnd the next one, empty slices
    // in between costing nothing. Both are clamped to what pattern_calc actually wrote: 0 at the
    // bottom, and at the top the grand total.
    //
    // PatternOffsets is an exclusive scan with the total appended one slot past the last curve, so
    // this curve's slice is the gap between neighbouring offsets and the total is a single load at
    // [TotalCurveCount]. It used to be a uint2 per curve plus a second read of the LAST curve's pair
    // to reassemble that total; the appended slot is the same number, already summed.
    const uint sliceBegin = PatternOffsets[curveIndex];
    const uint sliceEnd = PatternOffsets[curveIndex + 1u];
    const int patternTotal = (TotalCurveCount > 0u) ? int(PatternOffsets[TotalCurveCount]) : 0;

    const float curveArcBegin = WorldDistances[BezierData[curveIndex].FirstIndex];
    const int patternBase = (curveArcBegin > 0.0 && style.Spacing > 0.0)
        ? (int) floor(curveArcBegin / style.Spacing) + 1
        : 0;

    // An EMPTY slice needs no special case: its offset sits exactly where its centres would have
    // been, so lo/hi collapse onto the two centres bracketing the curve - the whole reason a curve
    // that owns nothing can still show the tail of its neighbour's dash. The one real special case
    // is a chain with no centres at all, where there is nothing to read and hi < lo says so.
    o.PatternSlot = int3(
        int(sliceBegin) - patternBase,
        (patternTotal > 0) ? max(int(sliceBegin) - 1, 0) : 0,
        (patternTotal > 0) ? min(int(sliceEnd), patternTotal - 1) : -1);

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
        // Dense tessellation keeps consecutive segments near-collinear, so this is the
        // overwhelmingly common case: plain miter, nothing else needed.
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

    // Index 4 (the join wedge) is the only vertex whose lateral is COMPUTED, and it sits in the
    // index >= 2 half where the local dir_BC_r points the OPPOSITE way from the +width_pixel side the
    // other four assert - so it was writing a sign-flipped lateral. Invisible while everything read
    // abs(SDF.x), except inside the wedge triangle, where the magnitude was being interpolated from a
    // wrong-signed corner. Negating puts all five on one convention.
    o.SDF.x = (index == 4u) ? (dot(offset, dir_BC_r) * -width_pixel)
                            : (((index & 1u) == 0u) ? width_pixel : -width_pixel);
    o.SDF.y = sdf;
    o.SDF.zw = float2(width_pixel, l_CB);

    o.Position.xy = mad((2.0 * width_pixel) / WH, offset, o.Position.xy);

    return o;
}
