// Carry pass of the segmented exclusive prefix sum: adds each block's scanned total to the elements that
// have no segment begin in front of them inside the block. Define SCAN_VALUE_TYPE, then include this -
// see segmented_scan_add_offsets_float.hlsl and segmented_scan_add_offsets_float2.hlsl.
#include "segmented_scan_common.hlsli"

//
// The block's first begin (FirstBegin, written by the local pass) is where the carry stops. At the top
// level the scan is restarted AT a begin, so the begin itself gets no carry: local < first. Below it the
// flags are the previous level's block flags and the result is shifted by one (exclusive), so the
// element at the first begin still gets it: local <= first.
[numthreads(GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    uint blockIndex = groupId.x;
    uint localIndex = groupThreadId.x;
    uint globalIndex = blockIndex * GROUP_SIZE + localIndex;

    if (globalIndex >= ElementCount) return;

    uint firstBegin = FirstBegin[blockIndex];
    bool needsCarry = (IsTopLevel != 0u) ? (localIndex < firstBegin) : (localIndex <= firstBegin);
    if (!needsCarry) return;

    Values[globalIndex] += BlockSums[blockIndex];
}
