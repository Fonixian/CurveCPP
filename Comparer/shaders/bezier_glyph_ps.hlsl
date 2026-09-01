// Glyph-mode counterpart of bezier_ps.hlsl.
//
// bezier_ps.hlsl works in the curve's own arc-length frame: x runs along the curve, y across it.
// That frame is curvilinear - it bends with the curve - which is exactly right for a dash and
// wrong for a shape. A heart placed in it gets warped on bends, and once the glyph is wider than
// the local radius of curvature the frame folds over itself and the shape pinches.
//
// So this shader ignores arc length entirely. pattern_center_glyph.hlsl gives every pattern a
// screen position and a tangent; a pixel transforms itself into that frame and evaluates the SDF
// there. The stamp is rigid: no bending, no fold-over, correct at any glyph size.
//
// Three neighbouring patterns are tested and unioned, so glyphs that overlap on screen merge
// instead of clipping each other at the halfway point.
struct VSOutputFinal {
    float4 Position : SV_Position;
    noperspective float4 SDF : TEXCOORD0;                // z = stroke half-width, unused here
    noperspective float4 Color : COLOR0;
    noperspective float TotalDistance : TEXCOORD1;       // Cumulative WORLD arc length
    nointerpolation float Spacing : TEXCOORD2;           // World-space pattern spacing
    nointerpolation float ScreenArcBegin : TEXCOORD3;    // unused here
    nointerpolation float DashLength : TEXCOORD4;        // unused here
    nointerpolation uint2 PatternRange : TEXCOORD5;      // x = first pattern index, y = count
    nointerpolation uint Style : TEXCOORD6;
    nointerpolation float GlyphPx : TEXCOORD7;           // Glyph half-extent in pixels
    nointerpolation uint GlyphKind : TEXCOORD8;
};

// xy = screen position of the pattern center, zw = unit screen tangent. Both y-down, matching
// SV_Position. A zero tangent means the center was behind the near plane.
StructuredBuffer<float4> PatternGlyphs : register(t1);

static const uint GlyphDot   = 0u;
static const uint GlyphArrow = 1u;
static const uint GlyphHeart = 2u;
static const uint GlyphStar  = 3u;

float Dot2(float2 v) { return dot(v, v); }

// Exact triangle SDF (Inigo Quilez).
float sdTriangle(float2 p, float2 p0, float2 p1, float2 p2)
{
    float2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    float2 v0 = p - p0,  v1 = p - p1,  v2 = p - p2;

    float2 pq0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    float2 pq1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    float2 pq2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);

    float s = sign(e0.x * e2.y - e0.y * e2.x);
    float2 d = min(min(float2(Dot2(pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                       float2(Dot2(pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                       float2(Dot2(pq2), s * (v2.x * e2.y - v2.y * e2.x)));

    return -sqrt(d.x) * sign(d.y);
}

// Heart SDF (Inigo Quilez), in its native frame: spans x [-0.603, 0.603], y [0.008, 1.103].
float sdHeartRaw(float2 p)
{
    p.x = abs(p.x);
    if (p.y + p.x > 1.0)
        return sqrt(Dot2(p - float2(0.25, 0.75))) - sqrt(2.0) / 4.0;

    return sqrt(min(Dot2(p - float2(0.0, 1.0)),
                    Dot2(p - 0.5 * max(p.x + p.y, 0.0)))) * sign(p.x - p.y);
}

// Five-pointed star SDF (Inigo Quilez). rf is the inner/outer radius ratio.
float sdStar5Raw(float2 p, float r, float rf)
{
    const float2 k1 = float2(0.809016994375, -0.587785252292);
    const float2 k2 = float2(-k1.x, k1.y);

    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;

    float2 ba = rf * float2(-k1.y, k1.x) - float2(0.0, 1.0);
    float  h  = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}

// Every glyph is normalised to a half-extent of 1 and centred on the origin, so GlyphSize means
// the same thing whichever shape is selected. The affine pre-transforms below were fitted to each
// shape's measured bounding box; dividing the result by the same scale keeps the return value a
// true distance, which the antialiasing ramp depends on.
float sdGlyph(float2 p, uint kind)
{
    if (kind == GlyphArrow)
    {
        // Points along +x, i.e. the direction of travel along the curve.
        return sdTriangle(p, float2(1.0, 0.0), float2(-0.7, 0.65), float2(-0.7, -0.65));
    }
    if (kind == GlyphHeart)
    {
        return sdHeartRaw(p * 0.603 + float2(0.0, 0.5555)) / 0.603;
    }
    if (kind == GlyphStar)
    {
        return sdStar5Raw(p * 0.95 + float2(0.0, 0.09), 1.0, 0.45) / 0.95;
    }
    return length(p) - 1.0; // GlyphDot
}

float4 main(VSOutputFinal input) : SV_Target
{
    if (input.PatternRange.y == 0u)
        discard;

    const float2 pixel = input.Position.xy; // already in pixels, y-down
    const float  scale = max(input.GlyphPx, 0.5);

    const uint base = input.PatternRange.x;
    const int  last = int(input.PatternRange.y) - 1;

    // World arc length gives the candidate pattern; the glyph may straddle its neighbours, so
    // take the union of three. Clamping can repeat an index at the ends, which is harmless.
    const int centre = clamp(int(floor(input.TotalDistance / max(input.Spacing, 1e-6))), 0, last);

    float d = 1e9;

    [unroll]
    for (int k = -1; k <= 1; ++k)
    {
        const float4 g = PatternGlyphs[base + uint(clamp(centre + k, 0, last))];

        if (g.z == 0.0 && g.w == 0.0)
            continue; // center was behind the near plane

        // Rotate into the glyph's own frame: +x along the tangent, +y to screen-up of it.
        const float2 v = pixel - g.xy;
        const float2 q = float2(dot(v, g.zw), dot(v, float2(g.w, -g.z)));

        d = min(d, sdGlyph(q / scale, input.GlyphKind) * scale);
    }

    // Antialias over the last pixel, same ramp as the arc-length shader.
    if (d > 0.5) discard;
    return float4(input.Color.rgb, saturate(0.5 - d));
}
