// Work-efficient (Blelloch) exclusive prefix sum over uint - shared declarations.
//
// One thread group scans ELEMENTS_PER_GROUP consecutive elements with
// THREAD_GROUP_SIZE threads, two elements per thread. An up-sweep builds a
// balanced reduction tree in groupshared memory and a down-sweep walks it back
// down into an exclusive scan, so a block costs O(n) adds instead of the
// O(n log n) a naive Hillis-Steele pass (see segmented_scan_local.hlsl) spends.
//
// This is what turns the per-curve pattern counts into per-curve base offsets:
// offset[i] = sum of counts[0..i-1], decided by curve order alone. The atomic
// InterlockedAdd it replaced handed out the same slices in whatever order the
// groups happened to retire, so the same scene could lay its patterns out
// differently from one run to the next.
//
// Groupshared indices are padded by CONFLICT_FREE_OFFSET. Both sweeps address
// the tree with a power-of-two stride, which unpadded would land every thread
// of a wave in the same LDS bank; one spare slot per bank-width skews the rows
// apart again.

#define THREAD_GROUP_SIZE  512u
#define ELEMENTS_PER_GROUP (THREAD_GROUP_SIZE * 2u)

#define LOG_BANK_COUNT 5u
#define CONFLICT_FREE_OFFSET(index) ((index) >> LOG_BANK_COUNT)
#define PADDED_SHARED_SIZE (ELEMENTS_PER_GROUP + (ELEMENTS_PER_GROUP >> LOG_BANK_COUNT))

cbuffer ScanConstants : register(b0) {
    uint ElementCount;
    uint3 Padding;
};

// Scanned in place. Unlike SegmentedScan there is no flag buffer: the running
// total crosses the whole range rather than restarting at segment boundaries.
RWStructuredBuffer<uint> Values    : register(u0);
RWStructuredBuffer<uint> BlockSums : register(u1);
