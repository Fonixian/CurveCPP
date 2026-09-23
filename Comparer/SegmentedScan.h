#pragma once
#include <Include/Axodox.Graphics.h>

using namespace Axodox::Graphics;

// An in-place EXCLUSIVE segmented prefix sum over a buffer of FLOATs, with the segment begins packed
// one bit per element into `flags`.
//
// The element type used to be float2, purely so BezierRenderer could scan its (world, screen) arc
// length pair in a single pass. It does not any more: it keeps the two channels in separate float
// buffers and owns two SegmentedScan instances, one per channel. Same total elements scanned, same
// bandwidth, but the scan itself is now a scalar merge - and bezier_dots, which only ever wanted
// world arc length, stops allocating and summing a second channel it filled with zeroes.
//
// An instance owns its block-sum / block-flag / carry-flag scratch, so scanning two buffers means
// two instances; one instance cannot be used for two concurrent scans.
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