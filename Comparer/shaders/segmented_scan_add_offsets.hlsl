#include "segmented_scan_common.hlsli"

[numthreads(GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    uint blockIndex = groupId.x;
    uint globalIndex = blockIndex * GROUP_SIZE + groupThreadId.x;

    if (globalIndex >= ElementCount) return;
    if (NeedsCarry[globalIndex] == 0u) return;

    Values[globalIndex] += BlockSums[blockIndex];
}
