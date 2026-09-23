#ifndef DOT_COMMON_HLSLI
#define DOT_COMMON_HLSLI

// K0..K3 are the curve in the MONOMIAL basis, NOT its control points:
//     P(t) = K0 + t * (K1 + t * (K2 + t * K3))
// The CPU converts once per upload (ToPowerBasis in bezier_common.cpp); the slots and the 80-byte
// layout are unchanged, so this costs nothing to load. Do not read K1 expecting P1.
struct BezierCurveData {
    float3 K0;
    int    FirstIndex;
    float3 K1;
    int    LastIndex;
    float3 K2;
    uint   ColorBegin;
    float3 K3;
    uint   ColorEnd;
    float  MinHeight;
    float  MaxHeight;
    float2 Padding;
};

struct DotStyle {
    float spacing;
    uint cap_cap_width;
};

// One dot: which two consecutive curve samples bracket it, and how far between them.
//
// CurveIndex is the curve those samples belong to. dot_calc has it in hand anyway and it fits in
// what used to be padding, so dot_vert can go straight to BezierData without the point -> curve
// index map - which matters now that dot_vert evaluates the curve itself rather than reading a
// position someone else stored.
struct DotSample {
    uint Sample;
    float SegmentT;
    uint CurveIndex;
};

static const uint CurveCapButt        = 0u;
static const uint CurveCapSquare      = 1u;
static const uint CurveCapRound       = 2u;
static const uint CurveCapTriangleOut = 3u;
static const uint CurveCapTriangleIn  = 4u;

float4 UnpackColorBits(uint packed) {
    return float4(
        float( packed        & 0xFF) / 255.0,
        float((packed >>  8) & 0xFF) / 255.0,
        float((packed >> 16) & 0xFF) / 255.0,
        float((packed >> 24) & 0xFF) / 255.0);
}

uint PackColorBits(float4 color) {
    uint r = uint(saturate(color.r) * 255.0 + 0.5);
    uint g = uint(saturate(color.g) * 255.0 + 0.5);
    uint b = uint(saturate(color.b) * 255.0 + 0.5);
    uint a = uint(saturate(color.a) * 255.0 + 0.5);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

uint FrontCap(uint cap_cap_width) { return (cap_cap_width >> 24) & 0xFF; }
uint BackCap(uint cap_cap_width) { return (cap_cap_width >> 16) & 0xFF; }
float Width(uint cap_cap_width) { return float(cap_cap_width & 0xFFFF) / 65535.0f * 500.0f; }

// Shared by dot_calc_points and dot_vert. The point pass evaluates the curve to measure chord
// lengths and throws the positions away; dot_vert evaluates the same curve again at the two samples
// bracketing a dot. Keeping one copy of the polynomial here is what makes "again" mean "identically"
// - two spellings of a cubic that agree algebraically can still disagree in the last bit, and a dot
// placed a hair off the arc length it was counted from is exactly the kind of drift that shows up as
// a seam.
//
// Horner over the monomial coefficients: three fused multiply-adds per component, against the
// Bernstein form's three weight products plus four scale-adds. The coefficients arrive that way
// already (see BezierCurveData above), so this is the whole evaluation.
float3 EvaluateBezier(float3 k0, float3 k1, float3 k2, float3 k3, float t)
{
    return mad(mad(mad(k3, t, k2), t, k1), t, k0);
}

// Curve parameter of one sample index. resolution - 1 intervals span t in [0, 1].
float SampleT(BezierCurveData bez, uint sampleIndex) {
    return float(sampleIndex - (uint)bez.FirstIndex) / float((uint)bez.LastIndex - (uint)bez.FirstIndex);
}

// How far along the C0 -> C1 ramp a point sits: world Y clamped to the height band when one is set,
// otherwise the curve parameter itself.
float ColorBlend(BezierCurveData bez, float3 position, float t) {
    if (bez.MinHeight < bez.MaxHeight)
        return saturate((position.y - bez.MinHeight) / (bez.MaxHeight - bez.MinHeight));
    return t;
}

#endif
