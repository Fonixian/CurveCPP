#include "segmented_scan_common.hlsli"

uint ReadFlag(uint index) {
    if (IsTopLevel != 0u) {
        uint packedWord = Flags[index >> 5u];
        return (packedWord >> (index & 31u)) & 1u;
    } else {
        return Flags[index];
    }
}

groupshared float gValue[2][GROUP_SIZE];
groupshared uint  gFlag[2][GROUP_SIZE];

[numthreads(GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    uint blockIndex = groupId.x;
    uint localIndex = groupThreadId.x;
    uint globalIndex = blockIndex * GROUP_SIZE + localIndex;
    bool active = globalIndex < ElementCount;

    uint blockCount = min(GROUP_SIZE, ElementCount - blockIndex * GROUP_SIZE);

    float originalValue = active ? Values[globalIndex] : 0.0f;
    uint originalFlag = active ? ReadFlag(globalIndex) : 0u;

    uint src = 0u;
    gValue[src][localIndex] = originalValue;
    gFlag[src][localIndex] = originalFlag;
    GroupMemoryBarrierWithGroupSync();

    for (uint offset = 1u; offset < GROUP_SIZE; offset <<= 1u) {
        uint dst = 1u - src;

        float value = gValue[src][localIndex];
        uint flag = gFlag[src][localIndex];

        if (localIndex >= offset) {
            float leftValue = gValue[src][localIndex - offset];
            uint leftFlag = gFlag[src][localIndex - offset];

            value = (flag != 0u) ? value : (leftValue + value);
            flag = leftFlag | flag;
        }

        gValue[dst][localIndex] = value;
        gFlag[dst][localIndex] = flag;
        src = dst;

        GroupMemoryBarrierWithGroupSync();
    }
    
    float finalValue;
    bool needsCarry;
    if (IsTopLevel != 0u) {
        finalValue = (localIndex == 0u || originalFlag != 0u) ? 0.0f : gValue[src][localIndex - 1u];
        needsCarry = gFlag[src][localIndex] == 0u;
    } else {
        finalValue = (localIndex == 0u) ? 0.0f : gValue[src][localIndex - 1u];
        needsCarry = (localIndex == 0u) ? true : (gFlag[src][localIndex - 1u] == 0u);
    }

    if (active) {
        Values[globalIndex] = finalValue;
        NeedsCarry[globalIndex] = needsCarry ? 1u : 0u;
    }

    if (localIndex == 0u) {
        uint lastLocalIndex = blockCount - 1u;
        BlockSums[blockIndex] = gValue[src][lastLocalIndex];
        BlockFlags[blockIndex] = gFlag[src][lastLocalIndex];
    }
}
