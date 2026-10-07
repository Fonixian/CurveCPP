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

// One dot: the sample it sits after, how far it is towards the next one, and the curve both belong to.
struct DotSample {
    uint Sample;
    float SegmentT;
    uint CurveIndex;
};

struct DotVSOutput {
    float4 Position : SV_Position;
    // x: lateral distance from the dot's center, y: distance along the tangent, z: half-width. All px.
    noperspective float3 SDF : TEXCOORD0;
    noperspective float4 Color : COLOR0;
    nointerpolation uint CapCapJoin : TEXCOORD1;
};

float4 UnpackColorBits(uint packed) {
    uint4 unpacked = uint4(packed, packed, packed, packed);
    unpacked >>= uint4(0, 8, 16, 24);
    unpacked &= 0xFF;
    return float4(unpacked) / 255.0;
}

struct Cubic {
    float3 P0, P1, P2, P3;
};

float3 EvaluateBezier(Cubic c, float t) {
    float s   = 1.0 - t;
    float st3 = 3.0 * s * t;
    return mad(c.P3, t * t * t, mad(c.P2, st3 * t, mad(c.P1, st3 * s, c.P0 * (s * s * s))));
}

float SampleT(uint2 range, uint sampleIndex) {
    return float(sampleIndex - range.x) / float(range.y - range.x);
}

float HalfWidth(uint style) { return float(style >> 24); }
uint FrontCap(uint capCapJoin) { return (capCapJoin >> 16) & 0xFF; }
uint BackCap(uint capCapJoin) { return (capCapJoin >> 8) & 0xFF; }

#endif
