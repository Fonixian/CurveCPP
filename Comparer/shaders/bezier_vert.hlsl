cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 wh;
};

// Per-curve appearance. Matches UploadCurveStyle in bezier_simple.cpp.
struct CurveStyle {
    float Width;           // Half-width (radius) of the stroke, in pixels
    uint  Style;           // 0 = Solid, 1 = Segmented
    float DashLength;      // Half-length of one dash, in pixels (arc-length mode)
    float GlyphSize;       // Glyph half-extent; world units if GlyphWorldSized, else pixels.
                           // ZERO in arc-length mode, which makes every glyph term below vanish.
    uint  GlyphKind;       // 0 = dot, 1 = arrow, 2 = heart, 3 = star
    uint  GlyphWorldSized; // Non-zero: GlyphSize is in world units and foreshortens
    float2 Padding;
};

// A glyph is a rigid stamp centred on the pattern, so the triangle strip has to be wide enough to
// contain it or it gets clipped by geometry that isn't there. The bounding box half-extent is 1
// after normalisation, so the corners reach sqrt(2); round up a little for the join cases.
static const float GlyphCoverage = 1.5;

StructuredBuffer<float4> CalculatedPoints  : register(t0);
StructuredBuffer<uint>   CurveBegins       : register(t1);
StructuredBuffer<float>  Distances         : register(t2); // Cumulative WORLD arc length per point
StructuredBuffer<float>  Spacing           : register(t3); // World-space pattern spacing per curve
StructuredBuffer<uint>   BezierIndexMap    : register(t4); // Packed: two uint16 curve indices per uint32
StructuredBuffer<float>  DistancesScreen   : register(t5); // Cumulative SCREEN arc length per point
StructuredBuffer<uint2>  PatternRanges     : register(t6); // Per curve: x = first pattern, y = pattern count
StructuredBuffer<CurveStyle> CurveStyles   : register(t7);

struct VSOutputFinal {
    float4 Position : SV_Position;
    // x: lateral distance from the centre line (px, signed)
    // y: longitudinal distance from point B ALONG the segment (px) - segment-local, 0 at B and
    //    l_CB at C. The pixel shader needs it segment-local for the join/cap rounding, and adds
    //    ScreenArcBegin to it to recover the curve-global screen arc length.
    // z: geomPx   w: l_CB (segment length in px)
    noperspective float4 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    noperspective float TotalDistance : TEXCOORD1;      // Cumulative WORLD arc length
    nointerpolation float Spacing : TEXCOORD2;          // World-space pattern spacing
    // Screen arc length at point B. Constant across the segment, so SDF.y + this is exact rather
    // than an interpolation of two per-vertex cumulative values.
    nointerpolation float ScreenArcBegin : TEXCOORD3;
    nointerpolation float DashLength : TEXCOORD4;
    nointerpolation uint2 PatternRange : TEXCOORD5;     // x = first pattern index, y = count
    nointerpolation uint Style : TEXCOORD6;
    // Read only by bezier_glyph_ps.hlsl. A pixel shader's input signature only has to be a subset
    // of the vertex shader's output, so bezier_ps.hlsl keeps linking against this unchanged.
    nointerpolation float GlyphPx : TEXCOORD7;          // Glyph half-extent, resolved to pixels
    nointerpolation uint GlyphKind : TEXCOORD8;
};

float4 UnpackColor(float packedAsFloat) {
    uint packed = asuint(packedAsFloat);
    float r = float(packed & 0xFF) / 255.0;
    float g = float((packed >> 8) & 0xFF) / 255.0;
    float b = float((packed >> 16) & 0xFF) / 255.0;
    float a = float((packed >> 24) & 0xFF) / 255.0;
    return float4(r, g, b, a);
}

bool IsCurveBegin(uint pointIndex) {
    uint packedWord = CurveBegins[pointIndex >> 5u];
    return ((packedWord >> (pointIndex & 31u)) & 1u) != 0u;
}

// BezierIndexMap stores two uint16 curve indices per uint32 - same layout, same helper as in
// bezier_calc_points.hlsl.
uint UnpackCurveIndex(uint pointIndex) {
    uint packed = BezierIndexMap[pointIndex >> 1];
    return (pointIndex & 1) ? (packed >> 16) : (packed & 0xFFFF);
}

VSOutputFinal main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    VSOutputFinal o = (VSOutputFinal)0;

    uint pointCount, pointStride;
    CalculatedPoints.GetDimensions(pointCount, pointStride);

    const uint segIdx = instanceId;

    const bool segmentValid = (segIdx + 1 < pointCount) && !IsCurveBegin(segIdx + 1);
    if (!segmentValid) {
        o.Position = float4(0, 0, 0, 0);
        return o;
    }

    const bool hasA = segIdx > 0 && !IsCurveBegin(segIdx);
    const bool hasD = (segIdx + 2) < pointCount && !IsCurveBegin(segIdx + 2);

    const uint curveIndex = UnpackCurveIndex(segIdx);
    const CurveStyle style = CurveStyles[curveIndex];

    float4 rawB = CalculatedPoints[segIdx];
    float4 rawC = CalculatedPoints[segIdx + 1];

    float4 B4 = mul(float4(rawB.xyz, 1.0), VP);
    float4 C4 = mul(float4(rawC.xyz, 1.0), VP);

    float4 colorB = UnpackColor(rawB.w);
    float4 colorC = UnpackColor(rawC.w);

    const float totalDistanceB = Distances[segIdx];
    const float totalDistanceC = Distances[segIdx + 1];

    {
        float t0 = B4.z;
        float t1 = C4.z;
        if (t0 < 0.0 && t1 < 0.0) {
            // Fully behind the camera: emit a degenerate vertex.
            o.Position = float4(0, 0, 0, 0);
            return o;
        }
        if (t0 < 0.0) {
            float t = t0 / (t0 - t1);
            B4 = lerp(B4, C4, t);
            colorB = lerp(colorB, colorC, t);
        } else if (t1 < 0.0) {
            float t = t1 / (t1 - t0);
            C4 = lerp(C4, B4, t);
            colorC = lerp(colorC, colorB, t);
        }
    }

    float4 A4 = float4(0, 0, 0, 0);
    if (hasA) {
        A4 = mul(float4(CalculatedPoints[segIdx - 1].xyz, 1.0), VP);
        float t1 = A4.z;
        float t0 = B4.z;
        if (t1 < 0.0) A4 = lerp(A4, B4, t1 / (t1 - t0));
        A4.xyz /= A4.w;
    }

    float4 D4 = float4(0, 0, 0, 0);
    if (hasD) {
        D4 = mul(float4(CalculatedPoints[segIdx + 2].xyz, 1.0), VP);
        float t1 = D4.z;
        float t0 = C4.z;
        if (t1 < 0.0) D4 = lerp(D4, C4, t1 / (t1 - t0));
        D4.xyz /= D4.w;
    }

    B4.xyz /= B4.w;
    C4.xyz /= C4.w;

    const float2 WH = wh;
    const float2 B = (B4.xy * 0.5 + 0.5) * WH;
    const float2 C = (C4.xy * 0.5 + 0.5) * WH;

    const float l_CB = max(distance(B, C), 1e-5);

    // World -> screen scale on this segment, straight out of the two arc lengths we already have.
    const float worldSeg = max(totalDistanceC - totalDistanceB, 1e-6);
    const float glyphPx = (style.GlyphWorldSized != 0u)
        ? style.GlyphSize * (l_CB / worldSeg)
        : style.GlyphSize;

    // Geometry half-width: wide enough for the stroke AND for the glyph stamped on it. In
    // arc-length mode GlyphSize is zero, so this collapses to style.Width and that path is
    // untouched - same vertices, same SDF range, same everything.
    const float geomPx = max(style.Width, glyphPx * GlyphCoverage);

    const float2 dir_CB = (C - B) / l_CB;
    const float2 dir_BC = -dir_CB;

    const float2 dir_r_CB = float2(-dir_CB.y, dir_CB.x);
    const float2 dir_r_BC = dir_r_CB;

    float2 begin_right_offset = -dir_CB + dir_r_CB;
    float2 begin_left_offset = -dir_CB - dir_r_CB;
    float2 end_right_offset = dir_CB + dir_r_CB;
    float2 end_left_offset = dir_CB - dir_r_CB;
    float2 end_third_offset = float2(0.0 / 0.0, 0.0 / 0.0);

    float2 sdf_begin = float2(-geomPx, -geomPx);
    float3 sdf_end = float3(l_CB + geomPx, l_CB + geomPx, l_CB + geomPx);
    float third_sdf_side = 0.0 / 0.0;

    if (hasA && l_CB > 1.0) {
        const float2 A = (A4.xy * 0.5 + 0.5) * WH;
        const float l_AB = max(distance(A, B), 1e-5);
        const float2 dir_AB = (A - B) / l_AB;
        const float2 dir_r_AB = float2(dir_AB.y, -dir_AB.x);

        const float2 dir_AB_r = (dot(dir_r_AB, dir_CB) < 0.0) ? -dir_r_AB : dir_r_AB;
        const float2 dir_BC_r = (dot(dir_r_BC, dir_AB) < 0.0) ? -dir_r_BC : dir_r_BC;
        const float2 inner_offset_ABC = (abs(dot(dir_AB, dir_BC)) >= 0.9999) ? dir_BC_r : (dir_AB_r + dir_BC_r) / (1.0 + dot(dir_AB_r, dir_BC_r));

        const float2 right_offset_ABC = (abs(dot(dir_AB, dir_BC)) > 0.9999) ? dir_r_AB : (dir_r_AB + dir_r_BC) / (1.0 + dot(dir_r_AB, dir_r_BC));

        const float2 dir_ABC = normalize(inner_offset_ABC);
        const float d_dot_ABC = dot(dir_AB, dir_BC);
        bool overlap_ABC;
        if (d_dot_ABC <= -0.9999) {
            overlap_ABC = true;
        } else if (d_dot_ABC >= 0.9999) {
            overlap_ABC = false;
        } else {
            const float cos_ABC = abs(dot(dir_AB, dir_ABC));
            const float l_ABC = geomPx * cos_ABC * rsqrt(max(1.0 - cos_ABC * cos_ABC, 1e-6));
            overlap_ABC = l_ABC > l_AB || l_ABC > l_CB;
        }

        if (d_dot_ABC >= 0.0) {
            // Obtuse join.
            begin_right_offset = right_offset_ABC;
            begin_left_offset = -right_offset_ABC;
        } else if (!overlap_ABC) {
            const bool rightIsInner = dot(right_offset_ABC, inner_offset_ABC) > 0.0;
            const float cos_half = clamp(dot(dir_r_BC, rightIsInner ? dir_ABC : -dir_ABC), -1.0, 1.0);
            const float t = (cos_half >= 1.0) ? 0.0 : sqrt((1.0 - cos_half) / (1.0 + cos_half));
            if (rightIsInner) {
                begin_right_offset = inner_offset_ABC;
                begin_left_offset = -dir_r_CB + t * dir_CB;
            } else {
                begin_left_offset = inner_offset_ABC;
                begin_right_offset = dir_r_BC + t * dir_BC;
            }
        }
        sdf_begin = float2(dot(begin_right_offset, dir_CB), dot(begin_left_offset, dir_CB)) * geomPx;
    }

    if (hasD) {
        const float2 D = (D4.xy * 0.5 + 0.5) * WH;
        const float l_DC = max(distance(D, C), 1e-5);
        const float2 dir_DC = (D - C) / l_DC;
        const float2 dir_r_DC = float2(-dir_DC.y, dir_DC.x);

        const float2 dir_CB_r = (dot(dir_r_CB, dir_DC) < 0.0) ? -dir_r_CB : dir_r_CB;
        const float2 dir_DC_r = (dot(dir_r_DC, dir_BC) < 0.0) ? -dir_r_DC : dir_r_DC;
        const float2 inner_offset_BCD = (abs(dot(dir_CB, dir_DC)) >= 0.9999) ? dir_DC_r : (dir_CB_r + dir_DC_r) / (1.0 + dot(dir_CB_r, dir_DC_r));

        const float2 right_offset_BCD = (abs(dot(dir_CB, dir_DC)) > 0.9999) ? dir_r_DC : (dir_r_CB + dir_r_DC) / (1.0 + dot(dir_r_CB, dir_r_DC));

        const float2 dir_BCD = normalize(inner_offset_BCD);
        const float d_dot_BCD = dot(dir_DC, dir_CB);
        bool overlap_BCD;
        if (d_dot_BCD <= -0.9999) {
            overlap_BCD = true;
        } else if (d_dot_BCD >= 0.9999) {
            overlap_BCD = false;
        } else {
            const float cos_BCD = abs(dot(dir_DC, dir_BCD));
            const float l_BCD = geomPx * cos_BCD * rsqrt(max(1.0 - cos_BCD * cos_BCD, 1e-6));
            overlap_BCD = l_BCD > l_DC || l_BCD > l_CB;
        }

        if (d_dot_BCD >= 0.0) {
            // Obtuse join.
            end_right_offset = right_offset_BCD;
            end_left_offset = -right_offset_BCD;
        } else if (!overlap_BCD) {
            const bool rightIsInner = dot(right_offset_BCD, inner_offset_BCD) > 0.0;
            const float cos_half = clamp(dot(dir_r_BC, rightIsInner ? dir_BCD : -dir_BCD), -1.0, 1.0);
            const float t = (cos_half >= 1.0) ? 0.0 : sqrt((1.0 - cos_half) / (1.0 + cos_half));
            if (rightIsInner) {
                end_right_offset = inner_offset_BCD;
                end_left_offset = -dir_r_CB + t * dir_CB;
                end_third_offset = -dir_r_DC - t * dir_DC;
            } else {
                end_left_offset = inner_offset_BCD;
                end_right_offset = dir_r_CB + t * dir_CB;
                end_third_offset = dir_r_DC - t * dir_DC;
            }
        }
        third_sdf_side = dot(end_third_offset, dir_r_CB) * geomPx;
        sdf_end = float3(dot(end_right_offset, dir_CB), dot(end_left_offset, dir_CB), dot(end_third_offset, dir_CB)) * geomPx;
        sdf_end += l_CB;
    }

    const float2 lengthConversion = 2.0 / WH * geomPx;

    float2 posXY;
    float posZ;
    float sdfX, sdfY;
    float4 vColor;
    float totalDistance;

    if (vertexId == 0) {
        posXY = mad(lengthConversion, begin_right_offset, B4.xy);
        posZ = B4.z;
        sdfX = geomPx;
        sdfY = sdf_begin.x;
        vColor = colorB;
        totalDistance = totalDistanceB;
    } else if (vertexId == 1) {
        posXY = mad(lengthConversion, begin_left_offset, B4.xy);
        posZ = B4.z;
        sdfX = -geomPx;
        sdfY = sdf_begin.y;
        vColor = colorB;
        totalDistance = totalDistanceB;
    } else if (vertexId == 2) {
        posXY = mad(lengthConversion, end_right_offset, C4.xy);
        posZ = C4.z;
        sdfX = geomPx;
        sdfY = sdf_end.x;
        vColor = colorC;
        totalDistance = totalDistanceC;
    } else if (vertexId == 3) {
        posXY = mad(lengthConversion, end_left_offset, C4.xy);
        posZ = C4.z;
        sdfX = -geomPx;
        sdfY = sdf_end.y;
        vColor = colorC;
        totalDistance = totalDistanceC;
    } else {
        posXY = mad(lengthConversion, end_third_offset, C4.xy);
        posZ = C4.z;
        sdfX = third_sdf_side;
        sdfY = sdf_end.z;
        vColor = colorC;
        totalDistance = totalDistanceC;
    }

    o.Position = float4(posXY, posZ, 1.0);
    // SDF.z stays the true stroke half-width - the arc-length shader measures the stroke body
    // against it, and it must not pick up the glyph widening applied to the geometry.
    o.SDF = float4(sdfX, sdfY, style.Width, l_CB);
    o.Color = vColor;
    o.TotalDistance = totalDistance;
    o.Spacing = Spacing[curveIndex];
    o.ScreenArcBegin = DistancesScreen[segIdx];
    o.DashLength = style.DashLength;
    o.PatternRange = PatternRanges[curveIndex];
    o.Style = style.Style;
    o.GlyphPx = glyphPx;
    o.GlyphKind = style.GlyphKind;
    return o;
}
