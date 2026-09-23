#include "paralell_scan_common.hlsli"

// By the time this runs, BlockSums has itself been scanned one level up, so
// BlockSums[i] is the sum of every element before block i. Adding it to each of
// the block's locally scanned values lifts them into the global scan.
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    const uint blockIndex = groupId.x;
    const uint blockBase = blockIndex * ELEMENTS_PER_GROUP;
    const uint blockOffset = BlockSums[blockIndex];

    // Same two-elements-per-thread split as the local pass.
    const uint globalA = blockBase + groupThreadId.x;
    const uint globalB = globalA + THREAD_GROUP_SIZE;

    if (globalA < ElementCount) Values[globalA] += blockOffset;
    if (globalB < ElementCount) Values[globalB] += blockOffset;
}
