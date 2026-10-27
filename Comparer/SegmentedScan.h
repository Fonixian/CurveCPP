#pragma once
#include <Include/Axodox.Graphics.h>

using namespace Axodox::Graphics;

// Names the two compute shaders a SegmentedScan<T> runs. Specialised in SegmentedScan.cpp, one per
// supported element type: add a specialisation, a pair of one-line wrappers in shaders\ that define
// SCAN_VALUE_TYPE (segmented_scan_local_<t>.hlsl, segmented_scan_add_offsets_<t>.hlsl), both vcxproj
// entries, and an explicit instantiation at the bottom of SegmentedScan.cpp.
//
// The names are string LITERALS on purpose: Pipeline::getCS caches by the const char* itself, not by
// its contents.
template <typename T>
struct SegmentedScanShaders;

// An in-place EXCLUSIVE segmented prefix sum over a buffer of T, with the segment begins packed one bit
// per element into `flags`, which is only read. T is the element type of `values`, of the per-block sums, and of the
// SCAN_VALUE_TYPE the shaders were compiled with - float and DirectX::XMFLOAT2 are instantiated.
// BezierRenderer scans its (world, screen) arc lengths together as one XMFLOAT2 buffer, the dots
// renderer scans world arc length alone as float.
//
// An instance owns its block-sum / block-flag / first-begin scratch, so one instance cannot be used for
// two concurrent scans.
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
        std::unique_ptr<RWStructuredBuffer> blockSums;
        std::unique_ptr<RWStructuredBuffer> blockFlags;
        std::unique_ptr<RWStructuredBuffer> firstBegin; // one uint per block, see FirstBegin in segmented_scan_common.hlsli
        std::unique_ptr<ConstantBuffer> constants;
    };

    GraphicsDevice _device;
    ComputeShader* _localScan;
    ComputeShader* _addBlockOffsets;
    std::vector<Level> _levels;

    // FlagBuffer is StructuredBuffer at the top level (the caller's begin bits) and RWStructuredBuffer
    // below it (the previous level's blockFlags: written there through a UAV, read here through its SRV).
    template <typename FlagBuffer>
    void ScanLevel(RWStructuredBuffer& values, FlagBuffer& flags, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context);
};
