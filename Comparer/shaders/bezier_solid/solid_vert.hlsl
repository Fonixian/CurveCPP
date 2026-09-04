#include "solid_common.hlsli"

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

bool calc_overlap(float2 dir_AB, float d, float2 v, float l_AB, float l_CB, float line_width) {
    if (d <= -0.9996) return true;
    if (d >= 0.9996) return false;

    float cos_abc = abs(dot(dir_AB, v));
    float sin_abc = rsqrt(max(0.0, 1.0 - cos_abc * cos_abc));
    float l = line_width * cos_abc * sin_abc;
    return l > l_AB || l > l_CB;
}

SolidVSOutput main(uint index : SV_VertexID, uint i : SV_InstanceID) {
    SolidVSOutput o = (SolidVSOutput)0;
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
    const float width_pixel = style.Width;
    o.Color = index < 2 ? float4(color_B, 1.0) : float4(color_C, 1.0);
    o.CapCapJoin = style.CapCapJoin;
    o.Neighbors = uint2(hasA ? 1u : 0u, hasD ? 1u : 0u);

    A4 /= A4.w;
    B4 /= B4.w;
    C4 /= C4.w;
    D4 /= D4.w;

    float2 A;
    float2 B;
    float2 C;
    if (index < 2) {
        A = A4.xy;
        B = B4.xy;
        C = C4.xy;
        o.Position = float4(B4.xyz, 1.0);
    } else {
        A = D4.xy;
        B = C4.xy;
        C = B4.xy;
        o.Position = float4(C4.xyz, 1.0);
    }

    A = mad(A, float2(0.5, 0.5), float2(0.5, 0.5)) * WH;
    B = mad(B, float2(0.5, 0.5), float2(0.5, 0.5)) * WH;
    C = mad(C, float2(0.5, 0.5), float2(0.5, 0.5)) * WH;

    A = any(isnan(A)) ? C : A;

    float l_AB = distance(A, B);
    float l_CB = distance(B, C);

    float2 dir_AB = (A - B) / l_AB;
    float2 dir_BC = (B - C) / l_CB;

    float2 dir_AB_r = float2(dir_AB.y, -dir_AB.x);
    float2 dir_BC_r = float2(dir_BC.y, -dir_BC.x);

    float2 inner; {
        float2 r_ab = dir_AB_r * (dot(dir_AB_r, dir_BC) >= 0.0 ? -1.0 : 1.0);
        float2 r_bc = dir_BC_r * (dot(dir_BC_r, dir_AB) < 0.0 ? -1.0 : 1.0);
        float den = dot(r_ab, r_bc);
        inner = den <= -0.9999 ? r_ab : (r_ab + r_bc) / (1.0 + den);
    }

    float d = dot(dir_AB, dir_BC);
    float2 right_offset = d <= -0.9999 ? dir_AB_r : (dir_AB_r + dir_BC_r) / (1.0 + d);
    float2 v = normalize(inner);
    bool overlap = calc_overlap(dir_AB, d, v, l_AB, l_CB, width_pixel);

    v *= (dot(right_offset, inner) <= 0.0 ? -1.0 : 1.0);
    float cos_half = clamp(dot(dir_BC_r, v), -1.0, 1.0);
    float t = sqrt(max(0.0, 1.0 - cos_half) / (1.0 + cos_half));

    float2 perp_base = index < 4 ? dir_BC_r : dir_AB_r;
    float2 perpendicular = (dot(right_offset, inner) >= 0.0 ? -1.0 : 1.0) * perp_base;
    float2 parallel = index < 4 ? dir_BC : -dir_AB;

    float2 no_overlap_offset; {
        float2 a = perpendicular + t * parallel;
        float2 b = index == 4 ? a : inner;
        if ((index > 1) != (index % 2 == 0)) {
            float2 tmp = a;
            a = b;
            b = tmp;
        }
        no_overlap_offset = dot(right_offset, inner) >= 0.0 ? a : b;
    }

    float s_12 = (index == 1 || index == 2) ? -1.0 : 1.0;
    float2 overlap_offset = dir_BC + s_12 * dir_BC_r;
    float2 obtuse_offset = s_12 * right_offset;

    float2 offset = dot(dir_AB, dir_BC) >= 0 ? obtuse_offset : (overlap ? overlap_offset : no_overlap_offset);
    float sdf = dot(offset, index < 2 ? dir_BC : -dir_BC) * -width_pixel;
    sdf += index < 2 ? 0.0 : l_CB;
    float2 length_conversion = 2.0 / WH * width_pixel;
    o.Position.xy = mad(length_conversion, offset, o.Position.xy);

    o.SDF.x = index == 4 ? (dot(offset, dir_BC_r) * width_pixel) : (index % 2 == 0 ? width_pixel : -width_pixel);
    o.SDF.y = sdf;
    o.SDF.zw = float2(width_pixel, l_CB);

    return o;
}