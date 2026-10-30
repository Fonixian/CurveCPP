#pragma once
#include <Include/Axodox.Graphics.h>

using namespace Axodox::Graphics;

// Names the two compute shaders a SegmentedScan<T> runs. Specialised in SegmentedScan.cpp, one per
// supported element type: add a specialisation, a pair of one-line wrappers in shaders\ that define
// SCAN_VALUE_TYPE (segmented_scan_local_<t>.hlsl, segmented_scan_reduce_<t>.hlsl), both vcxproj
// entries, and an explicit instantiation at the bottom of SegmentedScan.cpp.
//
// The names are string LITERALS on purpose: Pipeline::getCS caches by the const char* itself, not by
// its contents.
template <typename T>
struct SegmentedScanShaders;

// A compensated value, hi + lo with |lo| <= half an ulp of hi - the C++ twin of ScanPair in
// segmented_scan_common.hlsli. Both the scanned buffer and the per-level block sums hold these.
template <typename T>
struct ScanPair {
    T hi;
    T lo;
};

// An in-place EXCLUSIVE segmented prefix sum over a buffer of ScanPair<T>, with the segment begins packed
// one bit per element into `flags`, which is only read. T is the SCAN_VALUE_TYPE the shaders were
// compiled with - float and DirectX::XMFLOAT2 are instantiated. BezierRenderer scans its (world, screen)
// arc lengths together as ScanPair<XMFLOAT2>, the dots renderer world arc length alone as
// ScanPair<float>.
//
// The caller fills `values` with each element in hi and 0 in lo; the scan leaves each element's
// exclusive prefix sum there as a normalised compensated pair (~48 bits, see segmented_scan_common.hlsli
// and arc_pair.hlsli for how the consumers use it). Reduce-then-scan, two dispatches per
// level like before: reduce each block to its segmented total, scan the totals one level down, then
// scan each block with its carry added in and write the result once.
//
// An instance owns its block-sum / block-flag scratch, so one instance cannot be used for two
// concurrent scans.
template <typename T>
class SegmentedScan
{
public:
    static constexpr uint32_t GroupSize = 1024u;
    SegmentedScan(const GraphicsDevice&,uint32_t maxElementCount);
    void Scan(RWStructuredBuffer& values, StructuredBuffer& flags, uint32_t count, GraphicsDeviceContext* context = nullptr);

    SegmentedScan(const SegmentedScan&) = delete;
    SegmentedScan& operator=(const SegmentedScan&) = delete;
    SegmentedScan(SegmentedScan&&) = default;
    SegmentedScan& operator=(SegmentedScan&&) = default;
private:
    struct Level {
        uint32_t capacity = 0u;
        std::unique_ptr<RWStructuredBuffer> blockSums;  // ScanPair<T> per block: totals, then carries
        std::unique_ptr<RWStructuredBuffer> blockFlags; // uint per block: has a begin
        std::unique_ptr<ConstantBuffer> constants;
    };

    GraphicsDevice _device;
    ComputeShader* _localScan;
    ComputeShader* _reduce;
    std::vector<Level> _levels;

    // FlagBuffer is StructuredBuffer at the top level (the caller's begin bits) and RWStructuredBuffer
    // below it (the previous level's blockFlags: written there through a UAV, read here through its SRV).
    // `values` is the caller's buffer at the top level and the previous level's blockSums below it; both
    // are pairs and both bind at u0.
    template <typename FlagBuffer>
    void ScanLevel(RWStructuredBuffer& values, FlagBuffer& flags, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context);
};
