#ifndef SEGMENTED_SCAN_COMMON_HLSLI
#define SEGMENTED_SCAN_COMMON_HLSLI

// Compensated segmented exclusive prefix sum, written once per element type: the including .hlsl
// defines SCAN_VALUE_TYPE (float, float2) before including SegmentedScanLocal.hlsli or
// SegmentedScanReduce.hlsli. Every value is a (hi, lo) pair of SCAN_VALUE_TYPE - ScanPair<T> on the
// C++ side. The caller's buffer goes in with the element values in hi and 0 in lo, and comes back with
// each exclusive prefix sum as a normalised pair (see ArcPair.hlsli).
#ifndef SCAN_VALUE_TYPE
#error SCAN_VALUE_TYPE must be defined before including a segmented scan header
#endif

#define GROUP_SIZE 1024u  // elements per block, SegmentedScan<T>::GroupSize
#define SCAN_THREADS 512u // threads per block, two consecutive elements each

typedef SCAN_VALUE_TYPE ScanValue;

struct ScanPair
{
  ScanValue hi;
  ScanValue lo;
};

cbuffer ScanConstants : register(b0)
{
  uint ElementCount;
  uint IsTopLevel;
};

// The caller's packed begin bits at the top level, the previous level's BlockFlags below it.
StructuredBuffer<uint> Flags : register(t0);

// Scanned in place: the caller's buffer at the top level, the previous level's BlockSums below it.
RWStructuredBuffer<ScanPair> Values : register(u0);

// Per block of this level: the reduce pass writes each block's segmented total and whether it has a
// begin; once the level below has scanned them, BlockSums[b] is the carry into block b.
RWStructuredBuffer<ScanPair> BlockSums : register(u2);
RWStructuredBuffer<uint> BlockFlags : register(u3);

// TwoSum of the high parts with the low parts folded into the error, renormalised.
ScanPair ScanAdd(ScanPair a, ScanPair b)
{
  precise ScanValue s = a.hi + b.hi;
  precise ScanValue bv = s - a.hi;
  precise ScanValue av = s - bv;
  precise ScanValue e = ((a.hi - av) + (b.hi - bv)) + (a.lo + b.lo);
  precise ScanValue hi = s + e;
  precise ScanValue lo = e - (hi - s);
  ScanPair r;
  r.hi = hi;
  r.lo = lo;
  return r;
}

// left then right; a begin on the right restarts the sum.
ScanPair ScanCombine(ScanPair left, ScanPair right, uint rightFlag)
{
  if (rightFlag != 0u) return right;
  return ScanAdd(left, right);
}

uint ReadFlag(uint index)
{
  if (IsTopLevel != 0u)
  {
    uint packedWord = Flags[index >> 5u];
    return (packedWord >> (index & 31u)) & 1u;
  }
  return Flags[index];
}

#endif
