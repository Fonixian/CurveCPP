// Local (scan) pass of the compensated segmented exclusive prefix sum. Define SCAN_VALUE_TYPE, then
// include this - see segmented_scan_local_float.hlsl and segmented_scan_local_float2.hlsl.
//
// Reduce-then-scan: by the time this runs, segmented_scan_reduce has summed every block and the level
// below has scanned those sums, so BlockSums[block] is the running sum at the end of the previous block
// (the carry). This pass scans its block and adds the carry to every element that has no begin in
// front of it inside the block, then writes the final result once, as a pair. There is no separate
// carry pass: the carry is added in the same compensated arithmetic as everything else.
//
// Exclusive output. Top level: a begin element gets 0 (the scan restarts AT it). Below the top level
// the flags are block flags and the output is simply the running sum through the previous element,
// whose restart already happened inside that block's sum - same convention as before.
#include "segmented_scan_common.hlsli"

groupshared ScanPair gValue[2][SCAN_THREADS];
groupshared uint     gFlag[2][SCAN_THREADS];

[numthreads(SCAN_THREADS, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    const ScanPair zero = (ScanPair)0;

    uint blockIndex = groupId.x;
    uint thread = groupThreadId.x;
    uint index0 = blockIndex * GROUP_SIZE + 2u * thread;
    uint index1 = index0 + 1u;
    bool active0 = index0 < ElementCount;
    bool active1 = index1 < ElementCount;

    // Inactive elements are zeros without a begin: they change nothing that comes before them.
    ScanPair value0 = zero, value1 = zero;
    uint flag0 = 0u, flag1 = 0u;
    if (active0) { value0 = ReadValue(index0); flag0 = ReadFlag(index0); }
    if (active1) { value1 = ReadValue(index1); flag1 = ReadFlag(index1); }

    // This thread's two elements, inclusive.
    uint src = 0u;
    gValue[src][thread] = ScanCombine(value0, value1, flag1);
    gFlag[src][thread] = flag0 | flag1;
    GroupMemoryBarrierWithGroupSync();

    // Hillis-Steele over the threads' pairs: afterwards gValue[src][t] is the inclusive segmented sum of
    // the block's elements 0 .. 2t + 1, gFlag[src][t] whether a begin is among them.
    for (uint offset = 1u; offset < SCAN_THREADS; offset <<= 1u) {
        uint dst = 1u - src;

        ScanPair value = gValue[src][thread];
        uint flag = gFlag[src][thread];

        if (thread >= offset) {
            value = ScanCombine(gValue[src][thread - offset], value, flag);
            flag |= gFlag[src][thread - offset];
        }

        gValue[dst][thread] = value;
        gFlag[dst][thread] = flag;
        src = dst;

        GroupMemoryBarrierWithGroupSync();
    }

    // The running sum just before element 0 of this thread: the block's sum so far, plus the carry from
    // the earlier blocks unless a begin inside this block already cut it off.
    ScanPair before = zero;
    uint beforeFlag = 0u;
    if (thread > 0u) {
        before = gValue[src][thread - 1u];
        beforeFlag = gFlag[src][thread - 1u];
    }
    uint blockCount = (ElementCount + GROUP_SIZE - 1u) / GROUP_SIZE;
    if (beforeFlag == 0u && blockCount > 1u)
        before = ScanAdd(BlockSums[blockIndex], before);

    // Exclusive results: element 0 gets `before`, element 1 the running sum through element 0.
    ScanPair through0 = ScanCombine(before, value0, flag0);
    if (IsTopLevel != 0u) {
        if (flag0 != 0u) before = zero;
        if (flag1 != 0u) through0 = zero;
    }

    if (active0) WriteValue(index0, before);
    if (active1) WriteValue(index1, through0);
}
