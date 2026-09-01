#pragma once
#include <Include/Axodox.Graphics.h>

using namespace Axodox::Graphics;

class SegmentedScan
{
public:
    static constexpr uint32_t GroupSize = 1024u;
    SegmentedScan(const GraphicsDevice&,uint32_t maxElementCount);
    void Scan(RWStructuredBuffer& values, RWStructuredBuffer& flags, uint32_t count, GraphicsDeviceContext* context = nullptr);

    SegmentedScan(const SegmentedScan&) = delete;
    SegmentedScan& operator=(const SegmentedScan&) = delete;
    SegmentedScan(SegmentedScan&&) = default;
    SegmentedScan& operator=(SegmentedScan&&) = default;
private:
    struct Level {
        uint32_t capacity = 0u;
        std::unique_ptr<RWStructuredBuffer> blockSums;
        std::unique_ptr<RWStructuredBuffer> blockFlags;
        std::unique_ptr<RWStructuredBuffer> needsCarry;
        std::unique_ptr<ConstantBuffer> constants;
    };

    GraphicsDevice _device;
    ComputeShader* _localScan;
    ComputeShader* _addBlockOffsets;
    std::vector<Level> _levels;

    void ScanLevel(RWStructuredBuffer& values, RWStructuredBuffer& flags, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context);
};