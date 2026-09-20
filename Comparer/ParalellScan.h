#pragma once
#include <Include/Axodox.Graphics.h>

using namespace Axodox::Graphics;

class ParalellScan {
public:
    static constexpr uint32_t ThreadGroupSize = 256u;
    static constexpr uint32_t ElementsPerGroup = ThreadGroupSize * 2u;

    ParalellScan(const GraphicsDevice&, uint32_t maxElementCount);


    // Exclusive prefix sum over the first `count` elements, in place.
    //
    // With appendTotal, one more slot is written: values[count] comes back holding the sum of all
    // `count` elements, so the total is a plain load rather than a second pass reassembling it from
    // the last offset plus the last count. The caller's buffer must therefore have room for
    // count + 1 elements - a structured-buffer UAV drops an out-of-range store silently, so a buffer
    // sized to exactly `count` loses the total instead of faulting.
    void Scan(RWStructuredBuffer& values, uint32_t count, GraphicsDeviceContext* context, bool appendTotal = false);

    ParalellScan(const ParalellScan&) = delete;
    ParalellScan& operator=(const ParalellScan&) = delete;
    ParalellScan(ParalellScan&&) = default;
    ParalellScan& operator=(ParalellScan&&) = default;
private:
    struct Level {
        uint32_t capacity = 0u;
        std::unique_ptr<RWStructuredBuffer> blockSums;
        std::unique_ptr<ConstantBuffer> constants;
    };

    ComputeShader* _localScan;
    ComputeShader* _addBlockOffsets;
    std::vector<Level> _levels;

    void ScanLevel(RWStructuredBuffer& values, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context, bool appendTotal);
};
