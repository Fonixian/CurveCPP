#include "SegmentedScan.h"
#include "pipeline.h"
#ifdef PLATFORM_WINDOWS

using namespace Axodox::Graphics;

 struct ScanConstants {
    uint32_t ElementCount = 0u;
    uint32_t IsTopLevel = 0u;
 };

 uint32_t DivRoundUp(uint32_t value, uint32_t divisor) {
   return (value + divisor - 1u) / divisor;
 }

SegmentedScan::SegmentedScan(const GraphicsDevice& device, uint32_t maxElementCount) :
    _device(device),
    _localScan(Pipeline::getCS(device, "segmented_scan_local.cso")),
    _addBlockOffsets(Pipeline::getCS(device, "segmented_scan_add_offsets.cso")) {
    if (maxElementCount == 0u) throw std::invalid_argument("maxElementCount must be greater than zero.");

    uint32_t levelCapacity = maxElementCount;
    for (;;) {
        Level level;
        level.capacity = levelCapacity;
    
        auto blockCount = DivRoundUp(levelCapacity, GroupSize);
        level.blockSums = std::make_unique<RWStructuredBuffer>(device, TypedCapacityOrImmutableData<float>(blockCount));
        level.blockFlags = std::make_unique<RWStructuredBuffer>(device, TypedCapacityOrImmutableData<uint32_t>(blockCount));
        level.needsCarry = std::make_unique<RWStructuredBuffer>(device, TypedCapacityOrImmutableData<uint32_t>(levelCapacity));
        level.constants = std::make_unique<ConstantBuffer>(device, ScanConstants{});
    
        _levels.push_back(std::move(level));
    
        if (blockCount <= 1u) break;
        levelCapacity = blockCount;
    }
}

void SegmentedScan::Scan(RWStructuredBuffer& values, RWStructuredBuffer& flags, uint32_t count, GraphicsDeviceContext* context) {
    if (_levels.empty() || count > _levels.front().capacity) {
        throw std::out_of_range("count exceeds the maxElementCount this SegmentedScan was constructed with.");
    }

    if (count == 0u) return;

    if (!context) context = _device.ImmediateContext();

    ScanLevel(values, flags, count, 0, context);
    context->BindUnorderedAccessView(nullptr, 0);
    context->BindUnorderedAccessView(nullptr, 1);
    context->BindUnorderedAccessView(nullptr, 2);
    context->BindUnorderedAccessView(nullptr, 3);
    context->BindUnorderedAccessView(nullptr, 4);
}

void SegmentedScan::ScanLevel(RWStructuredBuffer& values, RWStructuredBuffer& flags, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context) {
    if (levelIndex >= _levels.size()) {
        throw std::logic_error("SegmentedScan: not enough recursion levels were preallocated for this element count.");
    }

    auto& level = _levels[levelIndex];
    auto blockCount = DivRoundUp(count, GroupSize);

    ScanConstants constants;
    constants.ElementCount = count;
    constants.IsTopLevel = (levelIndex == 0u) ? 1u : 0u;
    level.constants->Upload(constants, context);
    level.constants->Bind(ShaderStage::Compute, 0u, context);

    values.BindUnordered(0u, context);
    flags.BindUnordered(1u, context);
    level.blockSums->BindUnordered(2u, context);
    level.blockFlags->BindUnordered(3u, context);
    level.needsCarry->BindUnordered(4u, context);

    _localScan->Run({ blockCount, 1u, 1u }, context);

    if (blockCount > 1u) {
        ScanLevel(*level.blockSums, *level.blockFlags, blockCount, levelIndex + 1u, context);
        level.constants->Bind(ShaderStage::Compute, 0u, context);
        values.BindUnordered(0u, context);
        level.blockSums->BindUnordered(2u, context);
        level.needsCarry->BindUnordered(4u, context);
    
        _addBlockOffsets->Run({ blockCount, 1u, 1u }, context);
    }
    context->BindUnorderedAccessView(nullptr, 0u);
    context->BindUnorderedAccessView(nullptr, 1u);
    context->BindUnorderedAccessView(nullptr, 2u);
    context->BindUnorderedAccessView(nullptr, 3u);
    context->BindUnorderedAccessView(nullptr, 4u);
}
#endif
