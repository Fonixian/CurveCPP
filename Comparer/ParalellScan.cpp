#include "ParalellScan.h"
#include "pipeline.h"
#ifdef PLATFORM_WINDOWS

using namespace Axodox::Graphics;

// Anonymous: SegmentedScan.cpp declares its own ScanConstants and DivRoundUp at
// file scope, and these must not collide with them at link time.
namespace {
    struct ParalellScanConstants {
        uint32_t ElementCount = 0u;
        uint32_t Padding[3] = { 0u, 0u, 0u }; // constant buffers round up to 16 bytes
    };

    uint32_t DivRoundUp(uint32_t value, uint32_t divisor) {
        return (value + divisor - 1u) / divisor;
    }
}

ParalellScan::ParalellScan(const GraphicsDevice& device, uint32_t maxElementCount) :
    _device(device),
    _localScan(Pipeline::getCS(device, "paralell_scan_local.cso")),
    _addBlockOffsets(Pipeline::getCS(device, "paralell_scan_add_offsets.cso")) {
    if (maxElementCount == 0u) throw std::invalid_argument("maxElementCount must be greater than zero.");

    // Allocate every recursion level up front, so a Scan() never allocates.
    uint32_t levelCapacity = maxElementCount;
    for (;;) {
        Level level;
        level.capacity = levelCapacity;

        auto blockCount = DivRoundUp(levelCapacity, ElementsPerGroup);
        level.blockSums = std::make_unique<RWStructuredBuffer>(device, TypedCapacityOrImmutableData<uint32_t>(blockCount));
        level.constants = std::make_unique<ConstantBuffer>(device, ParalellScanConstants{});

        _levels.push_back(std::move(level));

        if (blockCount <= 1u) break;
        levelCapacity = blockCount;
    }
}

void ParalellScan::Scan(RWStructuredBuffer& values, uint32_t count, GraphicsDeviceContext* context) {
    if (_levels.empty() || count > _levels.front().capacity) {
        throw std::out_of_range("count exceeds the maxElementCount this ParalellScan was constructed with.");
    }

    if (count == 0u) return;

    if (!context) context = _device.ImmediateContext();

    ScanLevel(values, count, 0, context);
    context->BindUnorderedAccessView(nullptr, 0);
    context->BindUnorderedAccessView(nullptr, 1);
}

void ParalellScan::ScanLevel(RWStructuredBuffer& values, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context) {
    if (levelIndex >= _levels.size()) {
        throw std::logic_error("ParalellScan: not enough recursion levels were preallocated for this element count.");
    }

    auto& level = _levels[levelIndex];
    auto blockCount = DivRoundUp(count, ElementsPerGroup);

    ParalellScanConstants constants;
    constants.ElementCount = count;
    level.constants->Upload(constants, context);
    level.constants->Bind(ShaderStage::Compute, 0u, context);

    values.BindUnordered(0u, context);
    level.blockSums->BindUnordered(1u, context);

    // Exclusive scan within each block; each group leaves its own total in blockSums.
    _localScan->Run({ blockCount, 1u, 1u }, context);

    if (blockCount > 1u) {
        // Scanning the block totals in place turns blockSums[i] into the sum of
        // everything before block i - exactly the offset that block is missing.
        // The recursion rebinds slots 0 and 1, hence the rebind below.
        ScanLevel(*level.blockSums, blockCount, levelIndex + 1u, context);

        level.constants->Bind(ShaderStage::Compute, 0u, context);
        values.BindUnordered(0u, context);
        level.blockSums->BindUnordered(1u, context);

        _addBlockOffsets->Run({ blockCount, 1u, 1u }, context);
    }

    context->BindUnorderedAccessView(nullptr, 0u);
    context->BindUnorderedAccessView(nullptr, 1u);
}
#endif
