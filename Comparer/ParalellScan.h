#pragma once
#include <Include/Axodox.Graphics.h>

using namespace Axodox::Graphics;

// Work-efficient (Blelloch) exclusive prefix sum over uint - the unsegmented
// counterpart of SegmentedScan. There is no flag buffer: one running total
// crosses the whole range instead of restarting at each segment boundary.
//
// What it is for here: turning the per-curve pattern/dot counts into per-curve
// base offsets. offset[i] = counts[0] + ... + counts[i-1], which depends on
// curve order alone, so the same scene lays its patterns out identically on
// every run and on every machine. The InterlockedAdd this replaced handed out
// the same slices in whatever order the thread groups happened to retire.
// The price is one extra uint per curve plus this class's block-sum buffers -
// a scan needs somewhere to put the counts it consumes, an atomic does not.
//
// Each thread group scans ElementsPerGroup elements with ThreadGroupSize
// threads (two elements each) by an up-sweep/down-sweep over a groupshared
// reduction tree, costing O(n) adds where SegmentedScan's Hillis-Steele pass
// costs O(n log n). Every group leaves its own total behind; those totals are
// scanned by the same algorithm one level up and added back, recursively, so a
// single Scan() call covers any element count up to the maxElementCount given
// at construction.
//
// ElementsPerGroup matches SegmentedScan::GroupSize on purpose, so both scans
// cut the same data into the same blocks and their timings compare directly.
class ParalellScan
{
public:
    static constexpr uint32_t ThreadGroupSize = 512u;
    static constexpr uint32_t ElementsPerGroup = ThreadGroupSize * 2u;

    ParalellScan(const GraphicsDevice&, uint32_t maxElementCount);

    // Scans `values` in place: values[i] becomes the sum of values[0..i-1]. The
    // grand total is not written anywhere - it is values[count-1] plus that
    // element's original count, which the caller already has.
    void Scan(RWStructuredBuffer& values, uint32_t count, GraphicsDeviceContext* context = nullptr);

    ParalellScan(const ParalellScan&) = delete;
    ParalellScan& operator=(const ParalellScan&) = delete;
    ParalellScan(ParalellScan&&) = default;
    ParalellScan& operator=(ParalellScan&&) = default;
private:
    // One per recursion step: level 0 scans the caller's buffer, level 1 scans
    // level 0's block sums, and so on until a single group covers them all.
    struct Level {
        uint32_t capacity = 0u;
        std::unique_ptr<RWStructuredBuffer> blockSums;
        std::unique_ptr<ConstantBuffer> constants;
    };

    GraphicsDevice _device;
    ComputeShader* _localScan;
    ComputeShader* _addBlockOffsets;
    std::vector<Level> _levels;

    void ScanLevel(RWStructuredBuffer& values, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context);
};
