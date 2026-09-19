// Work-efficient (Blelloch) exclusive prefix sum - shared declarations.
//
// One thread group scans ELEMENTS_PER_GROUP consecutive elements with
// THREAD_GROUP_SIZE threads, two elements per thread. An up-sweep builds a
// balanced reduction tree in groupshared memory and a down-sweep walks it back
// down into an exclusive scan, so a block costs O(n) adds instead of the
// O(n log n) a naive Hillis-Steele pass (see segmented_scan_local.hlsl) spends.
//
// ELEMENTS_PER_GROUP deliberately matches SegmentedScan's GROUP_SIZE: the two
// scans then cut the same data into the same blocks and recurse to the same
// depth, so their timings compare directly.
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
RWStructuredBuffer<float2> Values    : register(u0);
RWStructuredBuffer<float2> BlockSums : register(u1);
