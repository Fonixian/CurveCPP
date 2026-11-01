// Scan pass: scans its block, adds the carry from the earlier blocks (BlockSums, already scanned by the
// level below) to the elements with no begin in front of them, and writes the exclusive result.
// At the top level a begin element gets 0.
#include "SegmentedScanCommon.hlsli"

groupshared ScanPair gValue[2][SCAN_THREADS];
groupshared uint gFlag[2][SCAN_THREADS];

[numthreads(SCAN_THREADS, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
  const ScanPair zero = (ScanPair)0;

  uint blockIndex = groupId.x;
  uint thread = groupThreadId.x;
  uint index0 = blockIndex * GROUP_SIZE + 2u * thread;
  uint index1 = index0 + 1u;
  bool active0 = index0 < ElementCount;
  bool active1 = index1 < ElementCount;

  ScanPair value0 = zero;
  ScanPair value1 = zero;
  uint flag0 = 0u;
  uint flag1 = 0u;
  if (active0)
  {
    value0 = Values[index0];
    flag0 = ReadFlag(index0);
  }
  if (active1)
  {
    value1 = Values[index1];
    flag1 = ReadFlag(index1);
  }

  uint src = 0u;
  gValue[src][thread] = ScanCombine(value0, value1, flag1);
  gFlag[src][thread] = flag0 | flag1;
  GroupMemoryBarrierWithGroupSync();

  // Inclusive over the threads: gValue[src][t] sums the block's elements 0 .. 2t + 1.
  for (uint offset = 1u; offset < SCAN_THREADS; offset <<= 1u)
  {
    uint dst = 1u - src;

    ScanPair value = gValue[src][thread];
    uint flag = gFlag[src][thread];

    if (thread >= offset)
    {
      value = ScanCombine(gValue[src][thread - offset], value, flag);
      flag |= gFlag[src][thread - offset];
    }

    gValue[dst][thread] = value;
    gFlag[dst][thread] = flag;
    src = dst;

    GroupMemoryBarrierWithGroupSync();
  }

  ScanPair before = zero;
  uint beforeFlag = 0u;
  if (thread > 0u)
  {
    before = gValue[src][thread - 1u];
    beforeFlag = gFlag[src][thread - 1u];
  }
  uint blockCount = (ElementCount + GROUP_SIZE - 1u) / GROUP_SIZE;
  if (beforeFlag == 0u && blockCount > 1u) before = ScanAdd(BlockSums[blockIndex], before);

  ScanPair through0 = ScanCombine(before, value0, flag0);
  if (IsTopLevel != 0u)
  {
    if (flag0 != 0u) before = zero;
    if (flag1 != 0u) through0 = zero;
  }

  if (active0) Values[index0] = before;
  if (active1) Values[index1] = through0;
}
