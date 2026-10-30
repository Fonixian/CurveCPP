// Reduce pass of the compensated segmented exclusive prefix sum. Define SCAN_VALUE_TYPE, then include
// this - see segmented_scan_reduce_float.hlsl and segmented_scan_reduce_float2.hlsl.
//
// One group per block: BlockSums[block] = the block's segmented total (the sum since its last begin, or
// the whole block when it has none), as a (hi, lo) pair, and BlockFlags[block] = whether it has a begin.
// The level below scans these in place into per-block carries for segmented_scan_local.
#include "segmented_scan_common.hlsli"

groupshared ScanPair gSum[SCAN_THREADS];
groupshared uint     gFlag[SCAN_THREADS];

[numthreads(SCAN_THREADS, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    const ScanPair zero = (ScanPair)0;

    uint blockIndex = groupId.x;
    uint thread = groupThreadId.x;
    uint index0 = blockIndex * GROUP_SIZE + 2u * thread;
    uint index1 = index0 + 1u;

    // Inactive elements are zeros without a begin, which leaves the total alone.
    ScanPair value0 = zero, value1 = zero;
    uint flag0 = 0u, flag1 = 0u;
    if (index0 < ElementCount) { value0 = ReadValue(index0); flag0 = ReadFlag(index0); }
    if (index1 < ElementCount) { value1 = ReadValue(index1); flag1 = ReadFlag(index1); }

    gSum[thread] = ScanCombine(value0, value1, flag1);
    gFlag[thread] = flag0 | flag1;
    GroupMemoryBarrierWithGroupSync();

    // Tree over neighbouring runs, left one first: the operator is not commutative, so a run is only
    // ever combined with the run directly after it.
    for (uint stride = 1u; stride < SCAN_THREADS; stride <<= 1u) {
        uint left = thread * 2u * stride;
        if (left + stride < SCAN_THREADS) {
            uint right = left + stride;
            gSum[left] = ScanCombine(gSum[left], gSum[right], gFlag[right]);
            gFlag[left] |= gFlag[right];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (thread == 0u) {
        BlockSums[blockIndex] = gSum[0];
        BlockFlags[blockIndex] = gFlag[0];
    }
}
