#include "ParalellScan.h"
#include "pipeline.h"
#ifdef PLATFORM_WINDOWS

using namespace Axodox::Graphics;

// Anonymous: SegmentedScan.cpp declares its own ScanConstants and DivRoundUp at
// file scope, and these must not collide with them at link time.
namespace {
    struct ParalellScanConstants {
        uint32_t ElementCount = 0u;           // slots written back
        uint32_t InputCount = 0u;             // slots holding input; the rest read as 0
        uint32_t Padding[2] = { 0u, 0u };     // constant buffers round up to 16 bytes
    };

    uint32_t DivRoundUp(uint32_t value, uint32_t divisor) {
        return (value + divisor - 1u) / divisor;
    }
}

ParalellScan::ParalellScan(const GraphicsDevice& device, uint32_t maxElementCount) :
    _localScan(Pipeline::getCS(device, "paralell_scan_local.cso")),
    _addBlockOffsets(Pipeline::getCS(device, "paralell_scan_add_offsets.cso")) {
    if (maxElementCount == 0u) throw std::invalid_argument("maxElementCount must be greater than zero.");

    // Allocate every recursion level up front, so a Scan() never allocates.
    //
    // The top level may be asked to write one element past `count` (Scan's appendTotal), and if
    // count is an exact multiple of ElementsPerGroup that slot falls into a block of its own, so its
    // block count comes from maxElementCount + 1. That block reads as all zeros and contributes a
    // zero block sum; only the offset added to it matters. Deeper levels never append.
    uint32_t levelCapacity = maxElementCount;
    bool topLevel = true;
    for (;;) {
        Level level;
        level.capacity = levelCapacity;

        auto blockCount = DivRoundUp(topLevel ? levelCapacity + 1u : levelCapacity, ElementsPerGroup);
        topLevel = false;
        level.blockSums = std::make_unique<RWStructuredBuffer>(device, TypedCapacityOrImmutableData<uint32_t>(blockCount));
        level.constants = std::make_unique<ConstantBuffer>(device, ParalellScanConstants{});

        _levels.push_back(std::move(level));

        if (blockCount <= 1u) break;
        levelCapacity = blockCount;
    }
}

void ParalellScan::Scan(RWStructuredBuffer& values, uint32_t count, GraphicsDeviceContext* context, bool appendTotal) {
    if (_levels.empty() || count > _levels.front().capacity) {
        throw std::out_of_range("count exceeds the maxElementCount this ParalellScan was constructed with.");
    }

    if (count == 0u) return;

    ScanLevel(values, count, 0, context, appendTotal);
    context->BindUnorderedAccessView(nullptr, 0);
    context->BindUnorderedAccessView(nullptr, 1);
}

void ParalellScan::ScanLevel(RWStructuredBuffer& values, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context, bool appendTotal) {
    if (levelIndex >= _levels.size()) {
        throw std::logic_error("ParalellScan: not enough recursion levels were preallocated for this element count.");
    }

    auto& level = _levels[levelIndex];

    // The appended total is one more slot to write, and blocks are counted over what gets written,
    // not over what gets read - otherwise a count that lands exactly on a block boundary would leave
    // the total's block undispatched.
    const uint32_t writtenCount = count + (appendTotal ? 1u : 0u);
    auto blockCount = DivRoundUp(writtenCount, ElementsPerGroup);

    ParalellScanConstants constants;
    constants.ElementCount = writtenCount;
    constants.InputCount = count;
    level.constants->Upload(constants, context);
    level.constants->Bind(ShaderStage::Compute, 0u, context);

    values.BindUnordered(0u, context);
    level.blockSums->BindUnordered(1u, context);

    // Exclusive scan within each block; each group leaves its own total in blockSums.
    _localScan->Run({ blockCount, 1u, 1u }, context);

    if (blockCount > 1u) {
        // Scanning the block totals in place turns blockSums[i] into the sum of
        // everything before block i - exactly the offset that block is missing.
        // The recursion rebinds slots 0 and 1, hence the rebind below. Block totals never get a
        // total of their own appended - only the caller's buffer does.
        ScanLevel(*level.blockSums, blockCount, levelIndex + 1u, context, false);

        level.constants->Bind(ShaderStage::Compute, 0u, context);
        values.BindUnordered(0u, context);
        level.blockSums->BindUnordered(1u, context);

        _addBlockOffsets->Run({ blockCount, 1u, 1u }, context);
    }

    context->BindUnorderedAccessView(nullptr, 0u);
    context->BindUnorderedAccessView(nullptr, 1u);
}
#endif
