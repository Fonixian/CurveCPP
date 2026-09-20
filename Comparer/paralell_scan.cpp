#include "ParalellScan.h"
#include "pipeline.h"

using namespace Axodox::Graphics;

struct ParalellScanConstants {
    int32_t ElementCount = 0;
    int32_t Padding[3] = { 0, 0, 0 };
};

ParalellScan::ParalellScan(const GraphicsDevice& device, uint32_t maxElementCount) :
    _localScan(Pipeline::getCS(device, "paralell_scan_local.cso")),
    _addBlockOffsets(Pipeline::getCS(device, "paralell_scan_add_offsets.cso")) {
    if (maxElementCount == 0u)
        throw std::invalid_argument("maxElementCount must be greater than zero.");

    uint32_t levelCapacity = maxElementCount;
    for (;;) {
        Level level;
        level.capacity = levelCapacity;

        auto blockCount = (levelCapacity + (ThreadGroupSize*2) - 1u) / (ThreadGroupSize*2);
        level.blockSums = std::make_unique<RWStructuredBuffer>(device, TypedCapacityOrImmutableData<uint32_t>(blockCount));
        level.constants = std::make_unique<ConstantBuffer>(device, ParalellScanConstants{});

        _levels.push_back(std::move(level));

        if (blockCount <= 1u) break;
        levelCapacity = blockCount;
    }
}

void ParalellScan::Scan(RWStructuredBuffer& values, uint32_t count, GraphicsDeviceContext* context) {
    if (count > _levels.front().capacity)
        throw std::out_of_range("count exceeds the maxElementCount this ParalellScan was constructed with.");

    if (count == 0u) return;

    ScanLevel(values, count, 0, context);
    context->BindUnorderedAccessView(nullptr, 0);
    context->BindUnorderedAccessView(nullptr, 1);
}

void ParalellScan::ScanLevel(RWStructuredBuffer& values, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context) {
    auto& level = _levels[levelIndex];
    auto blockCount = (count + (ThreadGroupSize * 2) - 1u) / (ThreadGroupSize*2);

    ParalellScanConstants constants;
    constants.ElementCount = count;
    level.constants->Upload(constants, context);
    level.constants->Bind(ShaderStage::Compute, 0u, context);

    values.BindUnordered(0u, context);
    level.blockSums->BindUnordered(1u, context);

    _localScan->Run({ blockCount, 1u, 1u }, context);
    if (blockCount > 1u) {
        ScanLevel(*level.blockSums, blockCount, levelIndex + 1u, context);

        level.constants->Bind(ShaderStage::Compute, 0u, context);
        values.BindUnordered(0u, context);
        level.blockSums->BindUnordered(1u, context);

        _addBlockOffsets->Run({ blockCount, 1u, 1u }, context);
    }

    context->BindUnorderedAccessView(nullptr, 0u);
    context->BindUnorderedAccessView(nullptr, 1u);
}