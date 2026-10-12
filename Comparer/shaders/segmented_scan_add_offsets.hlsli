// Carry pass of the segmented exclusive prefix sum: adds each block's scanned total to the elements that
// have no segment begin in front of them inside the block. Define SCAN_VALUE_TYPE, then include this -
// see segmented_scan_add_offsets_float.hlsl and segmented_scan_add_offsets_float2.hlsl.
#include "segmented_scan_common.hlsli"

[numthreads(GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    uint blockIndex = groupId.x;
    uint globalIndex = blockIndex * GROUP_SIZE + groupThreadId.x;

    if (globalIndex >= ElementCount) return;
    if (NeedsCarry[globalIndex] == 0u) return;

    Values[globalIndex] += BlockSums[blockIndex];
}
