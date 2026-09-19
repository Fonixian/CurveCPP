#include "paralell_scan_common.hlsli"

groupshared float2 gTree[PADDED_SHARED_SIZE];

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID) {
    const uint blockIndex = groupId.x;
    const uint localIndex = groupThreadId.x;
    const uint blockBase = blockIndex * ELEMENTS_PER_GROUP;

    // Two elements per thread, taken half a block apart so that neighbouring
    // threads still read neighbouring addresses.
    const uint localA = localIndex;
    const uint localB = localIndex + THREAD_GROUP_SIZE;
    const uint sharedA = localA + CONFLICT_FREE_OFFSET(localA);
    const uint sharedB = localB + CONFLICT_FREE_OFFSET(localB);

    const uint globalA = blockBase + localA;
    const uint globalB = blockBase + localB;

    // The tail block is zero padded up to a full ELEMENTS_PER_GROUP so the tree
    // stays a perfect power of two. Zero is the identity of the sum, so the
    // padding contributes nothing to the block total.
    gTree[sharedA] = (globalA < ElementCount) ? Values[globalA] : float2(0.0f, 0.0f);
    gTree[sharedB] = (globalB < ElementCount) ? Values[globalB] : float2(0.0f, 0.0f);

    // Up-sweep (reduce). After the pass with stride `offset`, the element at
    // k * offset - 1 holds the sum of the `offset` elements ending there, and
    // half as many threads stay active on each round.
    uint offset = 1u;
    for (uint upNodes = ELEMENTS_PER_GROUP >> 1u; upNodes > 0u; upNodes >>= 1u) {
        GroupMemoryBarrierWithGroupSync();

        if (localIndex < upNodes) {
            uint ai = offset * (2u * localIndex + 1u) - 1u;
            uint bi = offset * (2u * localIndex + 2u) - 1u;
            ai += CONFLICT_FREE_OFFSET(ai);
            bi += CONFLICT_FREE_OFFSET(bi);

            gTree[bi] += gTree[ai];
        }

        offset <<= 1u;
    }

    GroupMemoryBarrierWithGroupSync();

    // The root now holds the block total. Hand it to the next level, then clear
    // it: the down-sweep seeds the tree with the identity, which is what makes
    // the result exclusive rather than inclusive.
    const uint rootIndex = (ELEMENTS_PER_GROUP - 1u) + CONFLICT_FREE_OFFSET(ELEMENTS_PER_GROUP - 1u);
    if (localIndex == 0u) {
        BlockSums[blockIndex] = gTree[rootIndex];
        gTree[rootIndex] = float2(0.0f, 0.0f);
    }

    // Down-sweep. At each node the left child takes the parent's prefix and the
    // right child takes parent + left subtree sum, so one swap-and-add per node
    // turns the reduction tree into the scan. The loop's leading barrier also
    // covers the root clear above.
    for (uint downNodes = 1u; downNodes < ELEMENTS_PER_GROUP; downNodes <<= 1u) {
        offset >>= 1u;
        GroupMemoryBarrierWithGroupSync();

        if (localIndex < downNodes) {
            uint ai = offset * (2u * localIndex + 1u) - 1u;
            uint bi = offset * (2u * localIndex + 2u) - 1u;
            ai += CONFLICT_FREE_OFFSET(ai);
            bi += CONFLICT_FREE_OFFSET(bi);

            float2 leftPrefix = gTree[ai];
            gTree[ai] = gTree[bi];
            gTree[bi] += leftPrefix;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (globalA < ElementCount) Values[globalA] = gTree[sharedA];
    if (globalB < ElementCount) Values[globalB] = gTree[sharedB];
}
