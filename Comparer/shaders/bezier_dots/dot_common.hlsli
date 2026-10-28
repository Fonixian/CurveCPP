#ifndef DOT_COMMON_HLSLI
#define DOT_COMMON_HLSLI

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

struct DotStyle {
    uint  width_capcap; // width << 24 | front_cap << 16 | back_cap << 8; dots never join, so the low byte is 0
    float spacing;      // world arc length between dot centers; <= 0 means no dots on this curve
};

// One dot: where it sits on its curve, as the curve parameter t in [0, 1] over the WHOLE curve (not
// within one segment), and the curve it belongs to.
struct DotSample {
    float T;
    uint CurveIndex;
};

struct DotVSOutput {
    float4 Position : SV_Position;
    // Where the pixel is inside the dot's quad, in pixels from the dot's center: x across the tangent,
    // y along it, (+-QuadExtent, +-QuadExtent) at the corners.
    noperspective float2 Offset : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    nointerpolation uint CapCapJoin : TEXCOORD1;
};

// Inverse of PackFloat3ToR8G8B8A8 (DirectX::XMCOLOR): a << 24 | r << 16 | g << 8 | b.
float4 UnpackColorBits(uint packed) {
    uint4 unpacked = uint4(packed, packed, packed, packed);
    unpacked >>= uint4(16, 8, 0, 24);
    unpacked &= 0xFF;
    return float4(unpacked) / 255.0;
}

// One row per coordinate, one column per control point: row x = (P0.x, P1.x, P2.x, P3.x), likewise y
// and z, so P3 sits in the .w of every row. row_major so the rows sit in memory in that order, matching
// the CPU's XMFLOAT3X4 whatever the compiler's default matrix packing is. 48 B, three 16-byte loads.
struct Cubic {
    row_major float3x4 P;
};

float3 EvaluateBezier(Cubic c, float t) {
    float s   = 1.0 - t;
    float st3 = 3.0 * s * t;
    return mul(c.P, float4(s * s * s, st3 * s, st3 * t, t * t * t));
}

float3 EvaluateBezierDerivative(Cubic c, float t) {
    float s = 1.0 - t;
    // d/dt of the Bernstein weights: 3 * (-s^2, s^2 - 2st, 2st - t^2, t^2).
    return 3.0 * mul(c.P, float4(-s * s, s * (s - 2.0 * t), t * (2.0 * s - t), t * t));
}

float SampleT(uint2 range, uint sampleIndex) {
    return float(sampleIndex - range.x) / float(range.y - range.x);
}

#ifndef DOT_AA_RADIUS
#define DOT_AA_RADIUS 1.1
#endif

float HalfWidth(uint style) { return float(style >> 24); }
uint FrontCap(uint capCapJoin) { return (capCapJoin >> 16) & 0xFF; }
uint BackCap(uint capCapJoin) { return (capCapJoin >> 8) & 0xFF; }

#endif
