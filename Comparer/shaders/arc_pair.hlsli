#ifndef ARC_PAIR_HLSLI
#define ARC_PAIR_HLSLI

// Compensated arc lengths. The segmented scan (segmented_scan_common.hlsli) stores every cumulative arc
// length as an unevaluated sum hi + lo of two floats, normalised so |lo| <= half an ulp of hi - about 48
// significant bits. A pair is a float2 here: x = hi, y = lo.
//
// The point of keeping lo is that the consumers mostly SUBTRACT two cumulative arcs that are large and
// close together (a segment's length, a dash centre relative to a segment start, a curve-start phase on
// the spacing grid). In plain floats that difference is only good to an ulp of the large value - 0.06 px
// at a 1e6 px chain, 1 px at 1.8e7 px. Taken as pairs it is good to an ulp of the small result.
//
// Every intermediate is `precise`: without it the compiler is free to fold the error terms away
// ((s - a) - b is not 0 in floating point, which is the whole point) and to fuse products into mads.
// Needs round-to-nearest float add and multiply, which D3D11 requires.
//
// Comparisons and sign tests may look at hi alone where noted: for a normalised pair hi > 0 exactly
// when hi + lo > 0, and pairs order lexicographically by (hi, lo).

// a + b exactly, as a normalised pair (TwoSum).
float2 PairFromSum(float a, float b) {
    precise float s  = a + b;
    precise float bv = s - a;
    precise float e  = (a - (s - bv)) + (b - bv);
    return float2(s, e);
}

// a * b exactly, as a normalised pair (Dekker's TwoProduct with Veltkamp splitting - no fma needed).
float2 PairSplit(float a) {
    precise float c  = 4097.0 * a; // 2^12 + 1 splits a 24-bit mantissa into two 12-bit halves
    precise float hi = c - (c - a);
    precise float lo = a - hi;
    return float2(hi, lo);
}

float2 PairFromProduct(float a, float b) {
    precise float p = a * b;
    float2 as = PairSplit(a);
    float2 bs = PairSplit(b);
    precise float e = (((as.x * bs.x - p) + as.x * bs.y) + as.y * bs.x) + as.y * bs.y;
    return float2(p, e);
}

// The pair a plus the float b, normalised.
float2 PairAdd(float2 a, float b) {
    float2 s = PairFromSum(a.x, b);
    precise float e  = s.y + a.y;
    precise float hi = s.x + e;
    precise float lo = e - (hi - s.x);
    return float2(hi, lo);
}

// a - b rounded to one float: the high parts are differenced exactly (TwoSum), the low parts folded in.
float PairDiff(float2 a, float2 b) {
    float2 s = PairFromSum(a.x, -b.x);
    precise float d = s.x + (s.y + (a.y - b.y));
    return d;
}

// a <= b for normalised pairs.
bool PairLessEqual(float2 a, float2 b) {
    return (a.x < b.x) || (a.x == b.x && a.y <= b.y);
}

// floor(arc / spacing), exactly: the float quotient's floor can be one off when arc sits within an ulp of a
// grid point, so it is checked against the exact remainder arc - k * spacing. Every pass that maps an arc
// onto the spacing grid (counting, placing, the vertex shader's slot coordinate) goes through this one
// function, so they agree with each other exactly. spacing > 0, arc >= 0.
float PairGridIndex(float2 arc, float spacing) {
    float k = floor(arc.x / spacing);
    float remainder = PairDiff(arc, PairFromProduct(k, spacing));
    if (remainder < 0.0) k -= 1.0;
    else if (remainder >= spacing) k += 1.0;
    return k;
}

// How many grid points k * spacing (k = 0, 1, ...) lie in [0, arc]; 0 when the arc or spacing is 0.
uint PairGridCount(float2 arc, float spacing) {
    return (arc.x > 0.0 && spacing > 0.0) ? (uint)PairGridIndex(arc, spacing) + 1u : 0u;
}

#endif
