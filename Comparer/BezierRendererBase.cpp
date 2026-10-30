#include "BezierRendererBase.h"
#include "BezierCurve.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

using namespace Axodox::Graphics;
using namespace DirectX;

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
  cubic_points.insert(cubic_points.end(), { curve.P0, curve.P1, curve.P2, curve.P3 });
  color_data.push_back(UploadColorData{
    PackFloat3ToR8G8B8A8(curve.C0), PackFloat3ToR8G8B8A8(curve.C1),
    curve.min_height, curve.max_height
  });
  style_words.push_back(PackWidthCapCapJoin(curve));
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
      for (size_t i = 0; i < 4u; ++i) cubic_points[write * 4u + i] = cubic_points[read * 4u + i];
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

  cubic_points.resize(write * 4u);
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
  const auto points = cubic_points.begin() + std::ptrdiff_t(index * 4u);
  target.cubic_points.insert(target.cubic_points.end(), points, points + 4);
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

void BezierRendererBase::AllocateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context)
{
  CompactRemoved();

  // A merged curve shares its first sample with the previous curve.
  total_points = 0;
  for (size_t curveIndex = 0; curveIndex < TotalCurves(); ++curveIndex)
  {
    total_points += resolutions[curveIndex] - (IsChainStart(curveIndex) ? 0u : 1u);
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
    curve_begins.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::max((points_required + 31u) / 32u, 1u))));
    bezier_data_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(points_required)));
    AllocatePointBuffers(device, points_required);
    points_allocated = points_required;
  }

  if (curves_allocated != curves_required)
  {
    curve_control_points.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT3>(curves_required * 4u)));
    curve_colors.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadColorData>(curves_required)));
    curve_indices.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadSampleRange>(curves_required)));
    AllocateStyleBuffer(device, curves_required);
    AllocateCurveBuffers(device, curves_required);
    curves_allocated = curves_required;
  }

  std::vector<uint32_t> index_map;
  index_map.reserve(total_points);

  std::vector<uint32_t> curve_begin_bits(std::max((points_allocated + 31u) / 32u, 1u), 0u);

  uint32_t current = 0;
  for (uint32_t curveIndex = 0; curveIndex < curve_count; ++curveIndex)
  {
    const uint32_t resolution = resolutions[curveIndex];

    // One begin bit per chain, so arc lengths carry on across merged curves.
    if (IsChainStart(curveIndex))
    {
      curve_begin_bits[current / 32u] |= (1u << (current % 32u));
      for (unsigned i = 0; i < resolution; ++i)
      {
        index_map.push_back(curveIndex);
      }
      current += resolution;
    }
    else
    {
      // The joint sample belongs to the curve that starts there; the previous one still ends on it.
      index_map.back() = curveIndex;
      for (unsigned i = 1; i < resolution; ++i)
      {
        index_map.push_back(curveIndex);
      }
      current += resolution - 1u;
    }
  }
  assert(current == total_points);

  bezier_data_map->Upload(std::span<const uint32_t>{ index_map }, context);
  curve_begins->Upload(std::span<const uint32_t>{ curve_begin_bits }, context);

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

void BezierRendererBase::UpdatePatternBound()
{
  uint64_t bound = 0;
  for (size_t curveIndex = 0; curveIndex < owners.size(); ++curveIndex)
  {
    const XMFLOAT3* p = &cubic_points[curveIndex * 4u];
    bound += PatternCenterBound(p[0], p[1], p[2], p[3], spacings[curveIndex]);
  }
  pattern_upper_bound = static_cast<uint32_t>(std::min<uint64_t>(bound, maxPatternCount));
}

void BezierRendererBase::UploadCurves(GraphicsDeviceContext* context, uint8_t parts)
{
  if (owners.empty() || !curve_control_points) return;

  if (parts & DirtyPositions)
  {
#ifndef NDEBUG
    // Only the later curve's P0 is evaluated at a joint, so a gap would be closed silently.
    for (size_t curveIndex = 1; curveIndex < owners.size(); ++curveIndex)
    {
      assert((IsChainStart(curveIndex) || EndpointsMeet(cubic_points[curveIndex * 4u - 1u], cubic_points[curveIndex * 4u])) &&
        "merge_with_previous on a curve that does not start where the previous one ends");
    }
#endif
    curve_control_points->Upload(std::span<const XMFLOAT3>{ cubic_points }, context);
  }

  if (parts & DirtyColors)
  {
    curve_colors->Upload(std::span<const UploadColorData>{ color_data }, context);
  }

  if (parts & DirtyLayout)
  {
    index_scratch.resize(owners.size());
    uint32_t current = 0;
    for (size_t curveIndex = 0; curveIndex < owners.size(); ++curveIndex)
    {
      const uint32_t first = IsChainStart(curveIndex) ? current : current - 1u;
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
