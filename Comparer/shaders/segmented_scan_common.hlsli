#ifndef SEGMENTED_SCAN_COMMON_HLSLI
#define SEGMENTED_SCAN_COMMON_HLSLI

// The scan is written once and instantiated per element type. The including .hlsl defines
// SCAN_VALUE_TYPE (float, float2, ...) BEFORE including segmented_scan_local.hlsli or
// segmented_scan_reduce.hlsli; see segmented_scan_local_float.hlsl / _float2.hlsl. The C++ side
// (SegmentedScan<T>, SegmentedScan.cpp) picks the matching .cso; the caller's buffer and the block sums
// both hold ScanPair<T>, the C++ twin of ScanPair below.
//
// COMPENSATED, AND STORED AS PAIRS. Every value is an unevaluated (hi, lo) pair of SCAN_VALUE_TYPE
// (double-float: hi + lo, |lo| <= half an ulp of hi), combined with TwoSum. The caller's buffer is pairs
// too: it goes in with the element values in hi and 0 in lo, and comes back holding each exclusive
// prefix sum as a normalised pair - ~48 significant bits, so the sum of the input floats to ~1e-12
// relative whatever the summation tree. hi alone is that sum rounded to the nearest float (bar
// near-ties), so a consumer that only reads hi gets what the single-float version stored; the
// consumers that subtract two large arcs use both halves (arc_pair.hlsli). Output is monotonic for
// non-negative input and a zero-length step is exactly zero.
//
// Any type works that has component-wise `+`/`-` and a scalar-to-type cast `(SCAN_VALUE_TYPE)0`. The
// local pass keeps two groupshared copies of SCAN_THREADS pairs plus the flags: float2 needs 20 KB of
// the 32 KB limit, float4 would need 36 KB.
#ifndef SCAN_VALUE_TYPE
#error SCAN_VALUE_TYPE must be defined before including a segmented scan header
#endif

#define GROUP_SIZE   1024u   // elements per block - SegmentedScan<T>::GroupSize
#define SCAN_THREADS 512u    // threads per block, two consecutive elements each

typedef SCAN_VALUE_TYPE ScanValue;

struct ScanPair {
    ScanValue hi;
    ScanValue lo;
};

cbuffer ScanConstants : register(b0) {
    uint ElementCount;
    uint IsTopLevel;
};

// Flags is read-only: at the top level it is the caller's packed begin bits, below that the previous
// level's BlockFlags, which that level's reduce pass wrote and this one only reads.
StructuredBuffer<uint>        Flags      : register(t0);

// The buffer being scanned, in place, as pairs: at the top level the caller's buffer, below it the
// previous level's BlockSums.
RWStructuredBuffer<ScanPair>  Values     : register(u0);

// One per block of THIS level. The reduce pass writes each block's segmented total and whether it holds
// a begin; the level below scans them in place, after which BlockSums[b] is the carry into block b - the
// running sum at the end of block b - 1 - which the local pass adds in.
RWStructuredBuffer<ScanPair>  BlockSums  : register(u2);
RWStructuredBuffer<uint>      BlockFlags : register(u3);

// ---- compensated arithmetic --------------------------------------------------------------------
// `precise` is what keeps the compiler from simplifying the error terms away ((s - a) - b is not 0 in
// floating point, which is the whole point). Needs round-to-nearest float add, which D3D11 requires.

// a + b. TwoSum of the high parts (exact: s + e == a.hi + b.hi), the low parts folded into the error,
// then renormalised with Fast2Sum so hi is the pair rounded to nearest. Folding the low parts in with
// one add (the "sloppy" double-float add) keeps the full accuracy when they do not cancel, which holds
// for the non-negative arc lengths this scans; mixed signs would lose part of the extra precision.
ScanPair ScanAdd(ScanPair a, ScanPair b) {
    precise ScanValue s  = a.hi + b.hi;
    precise ScanValue bv = s - a.hi;
    precise ScanValue av = s - bv;
    precise ScanValue e  = ((a.hi - av) + (b.hi - bv)) + (a.lo + b.lo);
    precise ScanValue hi = s + e;
    precise ScanValue lo = e - (hi - s);
    ScanPair r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

// The segmented operator: (left, leftFlag) then (right, rightFlag). A begin on the right restarts.
ScanPair ScanCombine(ScanPair left, ScanPair right, uint rightFlag) {
    if (rightFlag != 0u) return right;
    return ScanAdd(left, right);
}

// ---- element access ----------------------------------------------------------------------------

uint ReadFlag(uint index) {
    if (IsTopLevel != 0u) {
        uint packedWord = Flags[index >> 5u];
        return (packedWord >> (index & 31u)) & 1u;
    } else {
        return Flags[index];
    }
}

ScanPair ReadValue(uint index) {
    return Values[index];
}

void WriteValue(uint index, ScanPair v) {
    Values[index] = v;
}

#endif
