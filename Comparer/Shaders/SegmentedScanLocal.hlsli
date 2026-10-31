// Local (per-block) pass of the segmented exclusive prefix sum. Define SCAN_VALUE_TYPE, then include
// this - see SegmentedScanLocalFloat.hlsl and SegmentedScanLocalFloat2.hlsl.
#include "SegmentedScanCommon.hlsli"

uint ReadFlag(uint index)
{
  if (IsTopLevel != 0u)
  {
    uint packedWord = Flags[index >> 5u];
    return (packedWord >> (index & 31u)) & 1u;
  }
  else
  {
    return Flags[index];
  }
}

groupshared SCAN_VALUE_TYPE gValue[2][GROUP_SIZE];
groupshared uint gFlag[2][GROUP_SIZE];

[numthreads(GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
  const SCAN_VALUE_TYPE zero = (SCAN_VALUE_TYPE)0;

  uint blockIndex = groupId.x;
  uint localIndex = groupThreadId.x;
  uint globalIndex = blockIndex * GROUP_SIZE + localIndex;
  bool active = globalIndex < ElementCount;

  uint blockCount = min(GROUP_SIZE, ElementCount - blockIndex * GROUP_SIZE);

  SCAN_VALUE_TYPE originalValue = active ? Values[globalIndex] : zero;
  uint originalFlag = active ? ReadFlag(globalIndex) : 0u;

  uint src = 0u;
  gValue[src][localIndex] = originalValue;
  gFlag[src][localIndex] = originalFlag;
  GroupMemoryBarrierWithGroupSync();

  for (uint offset = 1u; offset < GROUP_SIZE; offset <<= 1u)
  {
    uint dst = 1u - src;

    SCAN_VALUE_TYPE value = gValue[src][localIndex];
    uint flag = gFlag[src][localIndex];

    if (localIndex >= offset)
    {
      SCAN_VALUE_TYPE leftValue = gValue[src][localIndex - offset];
      uint leftFlag = gFlag[src][localIndex - offset];

      value = (flag != 0u) ? value : (leftValue + value);
      flag = leftFlag | flag;
    }

    gValue[dst][localIndex] = value;
    gFlag[dst][localIndex] = flag;
    src = dst;

    GroupMemoryBarrierWithGroupSync();
  }

  SCAN_VALUE_TYPE finalValue;
  if (IsTopLevel != 0u)
  {
    finalValue = (localIndex == 0u || originalFlag != 0u) ? zero : gValue[src][localIndex - 1u];
  }
  else
  {
    finalValue = (localIndex == 0u) ? zero : gValue[src][localIndex - 1u];
  }

  if (active)
  {
    Values[globalIndex] = finalValue;
  }

  // gFlag is now the inclusive OR of the flags, so it is 0 up to the block's first begin and 1 from
  // there on: the first begin is the one element where it steps from 0 to 1. Inactive elements have
  // no flag, so that element is always an active one. Exactly one thread writes FirstBegin - this one,
  // or thread 0 below when the block has no begin at all.
  uint lastLocalIndex = blockCount - 1u;
  if (gFlag[src][localIndex] != 0u && (localIndex == 0u || gFlag[src][localIndex - 1u] == 0u))
  {
    FirstBegin[blockIndex] = localIndex;
  }

  if (localIndex == 0u)
  {
    BlockSums[blockIndex] = gValue[src][lastLocalIndex];
    BlockFlags[blockIndex] = gFlag[src][lastLocalIndex];
    if (gFlag[src][lastLocalIndex] == 0u) FirstBegin[blockIndex] = GROUP_SIZE;
  }
}
