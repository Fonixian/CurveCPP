#include "SegmentedScan.h"
#include "pipeline.h"

using namespace Axodox::Graphics;

 struct ScanConstants {
    uint32_t ElementCount = 0u;
    uint32_t IsTopLevel = 0u;
 };

 static uint32_t DivRoundUp(uint32_t value, uint32_t divisor) {
   return (value + divisor - 1u) / divisor;
 }

template <>
struct SegmentedScanShaders<float> {
    static constexpr const char* Local = "segmented_scan_local_float.cso";
    static constexpr const char* Reduce = "segmented_scan_reduce_float.cso";
};

template <>
struct SegmentedScanShaders<DirectX::XMFLOAT2> {
    static constexpr const char* Local = "segmented_scan_local_float2.cso";
    static constexpr const char* Reduce = "segmented_scan_reduce_float2.cso";
};

template <typename T>
SegmentedScan<T>::SegmentedScan(const GraphicsDevice& device, uint32_t maxElementCount) :
    _device(device),
    _localScan(Pipeline::getCS(device, SegmentedScanShaders<T>::Local)),
    _reduce(Pipeline::getCS(device, SegmentedScanShaders<T>::Reduce)) {
    if (maxElementCount == 0u) throw std::invalid_argument("maxElementCount must be greater than zero.");

    uint32_t levelCapacity = maxElementCount;
    for (;;) {
        Level level;
        level.capacity = levelCapacity;

        auto blockCount = DivRoundUp(levelCapacity, GroupSize);
        level.blockSums = std::make_unique<RWStructuredBuffer>(device, TypedCapacityOrImmutableData<ScanPair<T>>(blockCount));
        level.blockFlags = std::make_unique<RWStructuredBuffer>(device, TypedCapacityOrImmutableData<uint32_t>(blockCount));
        level.constants = std::make_unique<ConstantBuffer>(device, ScanConstants{});

        _levels.push_back(std::move(level));

        if (blockCount <= 1u) break;
        levelCapacity = blockCount;
    }
}

static void BindFlags(StructuredBuffer& flags, GraphicsDeviceContext* context) { flags.Bind(ShaderStage::Compute, 0u, context); }
static void BindFlags(RWStructuredBuffer& flags, GraphicsDeviceContext* context) { flags.BindOrdered(ShaderStage::Compute, 0u, context); }

// Every slot the two scan shaders use: t0 flags, u0 values (pairs), u2 block sums, u3 block flags.
static void UnbindScanSlots(GraphicsDeviceContext* context) {
    context->BindShaderResourceView(nullptr, ShaderStage::Compute, 0u);
    context->BindUnorderedAccessView(nullptr, 0u);
    context->BindUnorderedAccessView(nullptr, 2u);
    context->BindUnorderedAccessView(nullptr, 3u);
}

template <typename T>
void SegmentedScan<T>::Scan(RWStructuredBuffer& values, StructuredBuffer& flags, uint32_t count, GraphicsDeviceContext* context) {
    if (_levels.empty() || count > _levels.front().capacity) {
        throw std::out_of_range("count exceeds the maxElementCount this SegmentedScan was constructed with.");
    }

    if (count == 0u) return;

    if (!context) context = _device.ImmediateContext();

    ScanLevel(values, flags, count, 0, context);
    UnbindScanSlots(context);
}

// Reduce-then-scan, recursively: with more than one block, reduce every block into level.blockSums /
// level.blockFlags, scan those one level down (which turns blockSums into each block's carry), then run
// the local scan, which adds the carries in. A single block needs only the local scan.
template <typename T>
template <typename FlagBuffer>
void SegmentedScan<T>::ScanLevel(RWStructuredBuffer& values, FlagBuffer& flags, uint32_t count, size_t levelIndex, GraphicsDeviceContext* context) {
    if (levelIndex >= _levels.size()) {
        throw std::logic_error("SegmentedScan: not enough recursion levels were preallocated for this element count.");
    }

    auto& level = _levels[levelIndex];
    auto blockCount = DivRoundUp(count, GroupSize);

    ScanConstants constants;
    constants.ElementCount = count;
    constants.IsTopLevel = (levelIndex == 0u) ? 1u : 0u;
    level.constants->Upload(constants, context);

    auto bindLevel = [&] {
        level.constants->Bind(ShaderStage::Compute, 0u, context);
        values.BindUnordered(0u, context);              // u0: the caller's pairs, or the level above's block sums
        BindFlags(flags, context);                      // t0, read-only
        level.blockSums->BindUnordered(2u, context);
        level.blockFlags->BindUnordered(3u, context);
    };
    bindLevel();

    if (blockCount > 1u) {
        _reduce->Run({ blockCount, 1u, 1u }, context);

        // The level below reads blockFlags through t0 and scans blockSums at u0, so clear the slots that
        // still hold them as UAVs (and everything else, so nothing from this level leaks into it), then
        // rebind this level for the local pass afterwards.
        UnbindScanSlots(context);
        ScanLevel(*level.blockSums, *level.blockFlags, blockCount, levelIndex + 1u, context);
        UnbindScanSlots(context);
        bindLevel();
    }

    _localScan->Run({ blockCount, 1u, 1u }, context);
}

template class SegmentedScan<float>;
template class SegmentedScan<DirectX::XMFLOAT2>;