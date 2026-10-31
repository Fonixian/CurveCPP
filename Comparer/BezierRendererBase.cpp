#include "BezierRendererBase.h"
#include "BezierCurve.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <execution>
#include <functional>
#include <limits>
#include <numeric>

using namespace Axodox::Graphics;
using namespace DirectX;

// bezier_data_map entry layout - keep in step with the *Common.hlsli files.
constexpr uint32_t SegmentHasPrev = 1u << 30;
constexpr uint32_t SegmentHasNext = 1u << 31;
constexpr uint32_t SegmentCurveMask = SegmentHasPrev - 1u;

// Fastest UpdatePatternBound loop seen so far, in nanoseconds per curve (the loop over every curve
// divided by the curve count, so renderers with different curve counts are comparable). Shared by all
// renderers; infinity until the first measurement or after a reset.
static double pattern_bound_min_ns = std::numeric_limits<double>::infinity();

double PatternBoundMinTimeNs()
{
  return pattern_bound_min_ns;
}

void ResetPatternBoundMinTime()
{
  pattern_bound_min_ns = std::numeric_limits<double>::infinity();
}

BezierRendererBase::BezierRendererBase(const GraphicsDevice& device) :
  profiler(device)
{
  viewport_data = std::make_unique<ConstantBuffer>(device, camera_cb_data);
}

BezierRendererBase::~BezierRendererBase()
{
  for (BezierCurve* owner : owners)
  {
    if (owner) owner->renderer = nullptr;
  }
}

void BezierRendererBase::BeginDraw()
{
  profiler.begin_frame();
  profiler.begin_cpu("total");
  profiler.begin_gpu("total");
}

void BezierRendererBase::EndDraw()
{
  profiler.end_gpu("total");
  profiler.end_cpu("total");
  profiler.end_frame();
}

void BezierRendererBase::SetViewport(float width, float height)
{
  camera_cb_data.wh = { width, height };
}

BezierCurve BezierRendererBase::Add(const BezierData& curve)
{
  assert(curve.resolution >= 2u);

  const size_t index = owners.size();
  cubic_points.push_back(XMFLOAT3X4{
    curve.P0.x, curve.P1.x, curve.P2.x, curve.P3.x,
    curve.P0.y, curve.P1.y, curve.P2.y, curve.P3.y,
    curve.P0.z, curve.P1.z, curve.P2.z, curve.P3.z
  });
  color_data.push_back(UploadColorData{
    PackFloat3ToR8G8B8A8(curve.C0), PackFloat3ToR8G8B8A8(curve.C1),
    curve.min_height, curve.max_height
  });
  // width << 24 | cap_front << 16 | cap_back << 8 | join - see style_words.
  style_words.push_back(
    (static_cast<uint32_t>(std::clamp(curve.width, 0.f, 255.f) + 0.5f) << 24) |
    (uint32_t(curve.cap_front) << 16) |
    (uint32_t(curve.cap_back) << 8) |
    uint32_t(curve.join));
  spacings.push_back(curve.spacing);
  dash_lengths.push_back(curve.dash_length);
  resolutions.push_back(curve.resolution);
  merged.push_back(curve.merge_with_previous ? 1u : 0u);
  owners.push_back(nullptr);
  need_resize = true;

  // If the return is not elided, the move constructor re-points owners[index].
  BezierCurve handle{ this, index };
  owners[index] = &handle;
  return handle;
}

void BezierRendererBase::Remove(BezierCurve& curve)
{
  if (curve.renderer == this) curve.Remove();
}

// Stable, so chains survive. A curve right after a removed one becomes a chain start.
void BezierRendererBase::CompactRemoved()
{
  if (removed_count == 0) return;

  size_t write = 0;
  bool previous_removed = false;
  for (size_t read = 0; read < owners.size(); ++read)
  {
    BezierCurve* owner = owners[read];
    if (!owner)
    {
      previous_removed = true;
      continue;
    }

    if (previous_removed) merged[read] = 0;
    previous_removed = false;

    if (write != read)
    {
      cubic_points[write] = cubic_points[read];
      color_data[write] = color_data[read];
      style_words[write] = style_words[read];
      spacings[write] = spacings[read];
      dash_lengths[write] = dash_lengths[read];
      resolutions[write] = resolutions[read];
      merged[write] = merged[read];
      owners[write] = owner;
    }
    owner->curve_index = write;
    ++write;
  }

  cubic_points.resize(write);
  color_data.resize(write);
  style_words.resize(write);
  spacings.resize(write);
  dash_lengths.resize(write);
  resolutions.resize(write);
  merged.resize(write);
  owners.resize(write);
  removed_count = 0;
}

void BezierRendererBase::MoveCurve(size_t index, BezierRendererBase& target, bool chain_start)
{
  BezierCurve* owner = owners[index];
  assert(owner && owner->renderer == this && &target != this);

  const size_t slot = target.owners.size();
  target.cubic_points.push_back(cubic_points[index]);
  target.color_data.push_back(color_data[index]);
  target.style_words.push_back(style_words[index]);
  target.spacings.push_back(spacings[index]);
  target.dash_lengths.push_back(dash_lengths[index]);
  target.resolutions.push_back(resolutions[index]);
  target.merged.push_back(chain_start ? uint8_t(0) : merged[index]);
  target.owners.push_back(owner);
  target.need_resize = true;

  owner->renderer = &target;
  owner->curve_index = slot;

  // As BezierCurve::Remove(), but the handle lives on in `target`.
  owners[index] = nullptr;
  ++removed_count;
  need_resize = true;
}

bool BezierRendererBase::IsChainStart(size_t index) const
{
  return index == 0 || merged[index] == 0;
}

void BezierRendererBase::AllocateSampleMap(const GraphicsDevice& device, uint32_t points_required)
{
  bezier_data_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(points_required)));
}

// One bit per sample, packed 32 to a word.
void BezierRendererBase::AllocateChainBegins(const GraphicsDevice& device, uint32_t points_required)
{
  curve_begins.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::max((points_required + 31u) / 32u, 1u))));
}

void BezierRendererBase::AllocateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context)
{
  CompactRemoved();

  total_points = 0;
  for (size_t curveIndex = 0; curveIndex < TotalCurves(); ++curveIndex)
  {
    total_points += resolutions[curveIndex];
  }

  const auto curve_count = static_cast<uint32_t>(TotalCurves());
  if (total_points < 2u || curve_count == 0u)
  {
    total_points = 0;
    pattern_upper_bound = 0;
    return;
  }

  const uint32_t points_required = FitCapacity(points_allocated, total_points);
  const uint32_t curves_required = FitCapacity(curves_allocated, curve_count);

  if (points_allocated != points_required)
  {
    AllocatePointBuffers(device, points_required);
    points_allocated = points_required;
  }

  if (curves_allocated != curves_required)
  {
    curve_control_points.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT3X4>(curves_required)));
    curve_colors.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadColorData>(curves_required)));
    curve_indices.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadSampleRange>(curves_required)));
    AllocateStyleBuffer(device, curves_required);
    AllocateCurveBuffers(device, curves_required);
    curves_allocated = curves_required;
  }

  std::vector<uint32_t> index_map;
  std::vector<uint32_t> curve_begin_bits;
  if (bezier_data_map) index_map.reserve(total_points);
  if (curve_begins) curve_begin_bits.assign(std::max((points_allocated + 31u) / 32u, 1u), 0u);

  uint32_t current = 0;
  for (uint32_t curveIndex = 0; curveIndex < curve_count; ++curveIndex)
  {
    const uint32_t resolution = resolutions[curveIndex];
    const bool chain_start = IsChainStart(curveIndex);

    // One begin bit per chain, so arc lengths carry on across merged curves. A merged curve still owns
    // all of its own samples, its first one being its own copy of the joint; the point pass measures a
    // zero chord from the previous curve's last sample to it, so the arc length runs on unbroken.
    if (curve_begins && chain_start)
    {
      curve_begin_bits[current / 32u] |= (1u << (current % 32u));
    }

    // Sample j starts the curve's segment j (j < resolution - 1). Inside the curve the stroke always
    // goes on; at its two ends only into a merged neighbour. The last sample starts no segment - the
    // step to the next curve - so it gets the bare curve index, and the draws cull its instance.
    if (bezier_data_map)
    {
      assert(curveIndex <= SegmentCurveMask);
      const bool joined_after = curveIndex + 1u < curve_count && !IsChainStart(curveIndex + 1u);
      for (uint32_t j = 0; j + 1u < resolution; ++j)
      {
        index_map.push_back(curveIndex |
          ((j > 0u || !chain_start) ? SegmentHasPrev : 0u) |
          ((j + 2u < resolution || joined_after) ? SegmentHasNext : 0u));
      }
      index_map.push_back(curveIndex);
    }

    current += resolution;
  }
  assert(current == total_points);
  assert(!bezier_data_map || index_map.size() == total_points);

  if (bezier_data_map) bezier_data_map->Upload(std::span<const uint32_t>{ index_map }, context);
  if (curve_begins) curve_begins->Upload(std::span<const uint32_t>{ curve_begin_bits }, context);

  UploadCurves(context, DirtyAll);
}

void BezierRendererBase::MarkForRouting(size_t index)
{
  if (route_all || (!route_marks.empty() && route_marks.back() == index)) return;
  if (route_marks.size() >= owners.size())
  {
    route_all = true;
    route_marks.clear();
    return;
  }
  route_marks.push_back(static_cast<uint32_t>(index));
}

bool BezierRendererBase::UpdateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context)
{
  profiler.begin_cpu("update");

  // BezierRenderer has routed by now; a compaction below would make the indices stale.
  route_marks.clear();
  route_all = false;

  bool updated = false;
  if (need_resize)
  {
    AllocateBuffers(device, context);
    need_resize = false;
    need_upload = false;
    upload_parts = 0;
    updated = true;
  }
  else if (need_upload)
  {
    UploadCurves(context, upload_parts ? upload_parts : static_cast<uint8_t>(DirtyAll));
    need_upload = false;
    upload_parts = 0;
    updated = true;
  }

  profiler.end_cpu("update");
  return updated;
}

void BezierRendererBase::UploadCameraData(const XMMATRIX& view_proj, GraphicsDeviceContext* context)
{
  XMStoreFloat4x4(&camera_cb_data.VP, XMMatrixTranspose(view_proj));
  camera_cb_data.TotalPointCount = total_points;
  camera_cb_data.TotalCurveCount = static_cast<uint32_t>(TotalCurves());
  viewport_data->Upload(camera_cb_data, context);
}

// Per curve floor(control polygon / spacing) + 1, or 0 when spacing <= 0: the polygon bounds the arc
// length, and a curve owns at most floor(own arc / spacing) + 1 centres of its chain's grid.
void BezierRendererBase::UpdatePatternBound()
{
  const auto start = std::chrono::steady_clock::now();
  const uint64_t bound = std::transform_reduce(std::execution::par_unseq,
    cubic_points.begin(), cubic_points.end(), spacings.begin(), uint64_t{ 0 },
    std::plus<>{},
    [](const XMFLOAT3X4& cubic, float spacing)
    {
      if (!(spacing > 0.f)) return static_cast<uint64_t>(0);

      // One row per coordinate, so the three edge vectors P1 - P0, P2 - P1, P3 - P2 come out of one
      // shifted subtract per row (the fourth lane is P3 - P3 = 0).
      const XMVECTOR x = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(cubic.m[0]));
      const XMVECTOR y = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(cubic.m[1]));
      const XMVECTOR z = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(cubic.m[2]));
      const XMVECTOR dx = XMVectorSubtract(XMVectorSwizzle<1, 2, 3, 3>(x), x);
      const XMVECTOR dy = XMVectorSubtract(XMVectorSwizzle<1, 2, 3, 3>(y), y);
      const XMVECTOR dz = XMVectorSubtract(XMVectorSwizzle<1, 2, 3, 3>(z), z);
      const XMVECTOR edges = XMVectorSqrt(XMVectorMultiplyAdd(dz, dz, XMVectorMultiplyAdd(dy, dy, XMVectorMultiply(dx, dx))));
      const float polygon = XMVectorGetX(XMVector4Dot(edges, XMVectorSplatOne()));

      const float centers = std::floor(polygon / spacing) + 1.f;
      constexpr float cap = static_cast<float>(maxPatternCount);
      return static_cast<uint64_t>(centers < cap ? centers : cap);
    });
  const auto elapsed = std::chrono::steady_clock::now() - start;

  if (!owners.empty())
  {
    const double ns_per_curve = std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(owners.size());
    pattern_bound_min_ns = std::min(pattern_bound_min_ns, ns_per_curve);
  }
  pattern_upper_bound = static_cast<uint32_t>(std::min<uint64_t>(bound, maxPatternCount));
}

void BezierRendererBase::UploadCurves(GraphicsDeviceContext* context, uint8_t parts)
{
  if (owners.empty() || !curve_control_points) return;

  if (parts & DirtyPositions)
  {
#ifndef NDEBUG
    // The step between the previous curve's last sample and this curve's first is taken to be
    // zero-length and never drawn, and the joins on both sides are built as if the stroke ran on
    // unbroken, so a gap would leave a hole.
    for (size_t curveIndex = 1; curveIndex < owners.size(); ++curveIndex)
    {
      assert((IsChainStart(curveIndex) || EndpointsMeet(cubic_points[curveIndex - 1u], cubic_points[curveIndex])) &&
        "merge_with_previous on a curve that does not start where the previous one ends");
    }
#endif
    // CPU time of the Map(WRITE_DISCARD) + copy + Unmap, recorded only on frames that upload.
    profiler.begin_cpu("upload points");
    curve_control_points->Upload(std::span<const XMFLOAT3X4>{ cubic_points }, context);
    profiler.end_cpu("upload points");
  }

  if (parts & DirtyColors)
  {
    profiler.begin_cpu("upload colors");
    curve_colors->Upload(std::span<const UploadColorData>{ color_data }, context);
    profiler.end_cpu("upload colors");
  }

  if (parts & DirtyLayout)
  {
    index_scratch.resize(owners.size());
    uint32_t current = 0;
    for (size_t curveIndex = 0; curveIndex < owners.size(); ++curveIndex)
    {
      const uint32_t first = current;
      const uint32_t last = first + resolutions[curveIndex] - 1u;
      index_scratch[curveIndex] = UploadSampleRange{ first, last };
      current = last + 1u;
    }
    assert(current == total_points);
    curve_indices->Upload(std::span<const UploadSampleRange>{ index_scratch }, context);
  }

  if (parts & DirtyStyles)
  {
    UploadStyles(context);
  }
}

void BezierRendererBase::BindCurveData(GraphicsDeviceContext* context,
  uint32_t control_point_slot, uint32_t color_slot, uint32_t index_slot)
{
  curve_control_points->Bind(ShaderStage::Vertex, control_point_slot, context);
  curve_colors->Bind(ShaderStage::Vertex, color_slot, context);
  curve_indices->Bind(ShaderStage::Vertex, index_slot, context);
}
