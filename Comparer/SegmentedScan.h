#pragma once
#include <Include/Axodox.Graphics.h>

using namespace Axodox::Graphics;

// Names the two compute shaders a SegmentedScan<T> runs. Specialised in SegmentedScan.cpp, one per
// supported element type: add a specialisation, a pair of one-line wrappers in Shaders\ that define
// SCAN_VALUE_TYPE (SegmentedScanLocal<Type>.hlsl, SegmentedScanReduce<Type>.hlsl), both vcxproj
// entries, and an explicit instantiation at the bottom of SegmentedScan.cpp.
//
// The names are string LITERALS on purpose: Pipeline::getCS caches by the const char* itself, not by
// its contents.
template<typename T>
struct SegmentedScanShaders;

// A compensated value hi + lo, |lo| <= half an ulp of hi. ScanPair in SegmentedScanCommon.hlsli.
template<typename T>
struct ScanPair
{
  T hi;
  T lo;
};

// An in-place EXCLUSIVE segmented prefix sum over a buffer of ScanPair<T>, with the segment begins packed
// one bit per element into `flags`, which is only read. float and DirectX::XMFLOAT2 are instantiated.
// The caller writes each element into hi with lo = 0; the scan leaves each exclusive prefix sum there
// as a normalised pair. Reduce-then-scan: reduce every block, scan the block totals one level down, then
// scan each block with its carry added in.
//
// An instance owns its block-sum / block-flag scratch, so one instance cannot be used for two
// concurrent scans.
template<typename T>
class SegmentedScan
{
public:
  static constexpr uint32_t GroupSize = 1024u;
  SegmentedScan(const GraphicsDevice&, uint32_t maxElementCount);
  void Scan(RWStructuredBuffer& values, StructuredBuffer& flags, uint32_t count, GraphicsDeviceContext* context = nullptr);

  SegmentedScan(const SegmentedScan&) = delete;
  SegmentedScan& operator=(const SegmentedScan&) = delete;
  SegmentedScan(SegmentedScan&&) = default;
  SegmentedScan& operator=(SegmentedScan&&) = default;

private:
  struct Level
  {
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
  // `values` is the caller's buffer at the top level and the previous level's blockSums below it.
  template<typename FlagBuffer>
  void ScanLevel(RWStructuredBuffer& values, FlagBuffer& flags, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context);
};
