#ifndef SEGMENTED_SCAN_COMMON_HLSLI
#define SEGMENTED_SCAN_COMMON_HLSLI

// The scan is written once and instantiated per element type. The including .hlsl defines
// SCAN_VALUE_TYPE (float, float2, ...) BEFORE including segmented_scan_local.hlsli or
// segmented_scan_add_offsets.hlsli; see segmented_scan_local_float.hlsl / _float2.hlsl. The C++ side
// (SegmentedScan<T>, SegmentedScan.cpp) picks the matching .cso, and its block-sum buffers use the same
// element type.
//
// Any type works that has component-wise `+`, a scalar-to-type cast `(SCAN_VALUE_TYPE)0` and lives in
// groupshared memory: two groupshared copies of GROUP_SIZE values plus the flags must stay under the 32 KB
// limit, which float4 would already break.
#ifndef SCAN_VALUE_TYPE
#error SCAN_VALUE_TYPE must be defined before including a segmented scan header
#endif

#define GROUP_SIZE 1024u

cbuffer ScanConstants : register(b0) {
    uint ElementCount;
    uint IsTopLevel;
};

// Flags is read-only: at the top level it is the caller's packed begin bits, below that the previous
// level's BlockFlags, which that level's pass wrote and this one only reads. Only the local pass binds it.
StructuredBuffer<uint>              Flags      : register(t0);

RWStructuredBuffer<SCAN_VALUE_TYPE> Values     : register(u0);
RWStructuredBuffer<SCAN_VALUE_TYPE> BlockSums  : register(u2);
RWStructuredBuffer<uint>            BlockFlags : register(u3);
// One per block: the local index of the block's first segment begin (its first set flag), GROUP_SIZE
// when the block has none. The carry pass adds the block's sum to every element before it - see
// segmented_scan_add_offsets.hlsli.
RWStructuredBuffer<uint>            FirstBegin : register(u4);

#endif
