#include "bezier_common.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <execution>
#include <limits>
#include <numeric>

using namespace Axodox::Graphics;
using namespace DirectX;

constexpr uint32_t SegmentHasPrev = 1u << 30;
constexpr uint32_t SegmentHasNext = 1u << 31;
constexpr uint32_t SegmentCurveMask = SegmentHasPrev - 1u;

uint32_t FitCapacity(uint32_t allocated, uint32_t required) {
	uint32_t fitted = required > 0u ? 1u << (std::numeric_limits<unsigned>::digits - std::countl_zero(required - 1u)) : 1u;
	if (allocated < fitted || fitted <= allocated / 4u) return fitted;
	return allocated;
}

void UnbindShaderResources(GraphicsDeviceContext* context, ShaderStage stage, std::span<const uint32_t> slots) {
	for (uint32_t slot : slots)
		context->BindShaderResourceView(nullptr, stage, slot);
}

void UnbindShaderResources(GraphicsDeviceContext* context, ShaderStage stage, std::initializer_list<uint32_t> slots) {
	UnbindShaderResources(context, stage, std::span<const uint32_t>(slots.begin(), slots.size()));
}

void UnbindUnorderedAccessViews(GraphicsDeviceContext* context, std::span<const uint32_t> slots) {
	for (uint32_t slot : slots)
		context->BindUnorderedAccessView(nullptr, slot);
}

void UnbindUnorderedAccessViews(GraphicsDeviceContext* context, std::initializer_list<uint32_t> slots) {
	UnbindUnorderedAccessViews(context, std::span<const uint32_t>(slots.begin(), slots.size()));
}


BezierRendererBase::BezierRendererBase(const GraphicsDevice& device) : profiler(device) {
	viewport_data = std::make_unique<ConstantBuffer>(device, camera_cb_data);
}

BezierRendererBase::~BezierRendererBase() {
	for (BezierCurve* owner : owners)
		if (owner) owner->renderer = nullptr;
}

void BezierRendererBase::BeginDraw() {
	profiler.begin_frame();
	profiler.begin_cpu("total");
	profiler.begin_gpu("total");
}

void BezierRendererBase::EndDraw() {
	profiler.end_gpu("total");
	profiler.end_cpu("total");
	profiler.end_frame();
}

void BezierRendererBase::SetViewport(float width, float height) { camera_cb_data.wh = { width, height }; }

BezierCurve BezierRendererBase::Add(const BezierData& curve) {
	assert(curve.resolution >= 2u);

	const size_t index = owners.size();
	// Rows x / y / z, columns P0..P3 - see cubic_points.
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

	// Registered from inside the handle's own storage. If the return is not elided, the move
	// constructor re-points owners[index] at the moved-to handle, so either way it ends up right.
	BezierCurve handle{ this, index };
	owners[index] = &handle;
	return handle;
}

void BezierRendererBase::Remove(BezierCurve& curve) {
	if (curve.renderer == this) curve.Remove();
}

// One pass, stable order - chains depend on it. Survivors slide down over the removed slots and
// have their handle's index rewritten through `owners`. A survivor whose immediate predecessor was
// removed is made a chain start: removal splits a chain rather than bridging the gap.
void BezierRendererBase::CompactRemoved() {
	if (removed_count == 0) return;

	size_t write = 0;
	bool previous_removed = false;
	for (size_t read = 0; read < owners.size(); ++read) {
		BezierCurve* owner = owners[read];
		if (!owner) {
			previous_removed = true;
			continue;
		}

		if (previous_removed) merged[read] = 0;
		previous_removed = false;

		if (write != read) {
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

// Fastest UpdatePatternBound loop seen so far, in nanoseconds PER CURVE (the loop over every curve
// divided by the curve count, so renderers with different curve counts are comparable). A single
// PatternCenterBound call is a few ns - below the clock's resolution - so the loop is what gets timed.
// Shared by all renderers; infinity until the first measurement or after a reset.
static double pattern_bound_min_ns = std::numeric_limits<double>::infinity();

double PatternBoundMinTimeNs() { return pattern_bound_min_ns; }
void ResetPatternBoundMinTime() { pattern_bound_min_ns = std::numeric_limits<double>::infinity(); }

bool BezierRendererBase::IsChainStart(size_t index) const {
	return index == 0 || merged[index] == 0;
}

void BezierRendererBase::AllocateSampleMap(const GraphicsDevice& device, uint32_t points_required) {
	bezier_data_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(points_required)));
}

// Always fewer segments than samples, so the point capacity fits the segment map too.
void BezierRendererBase::AllocateSegmentMap(const GraphicsDevice& device, uint32_t points_required) {
	segment_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(points_required)));
}

// One bit per sample, packed 32 to a word.
void BezierRendererBase::AllocateChainBegins(const GraphicsDevice& device, uint32_t points_required) {
	curve_begins.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::max((points_required + 31u) / 32u, 1u))));
}

void BezierRendererBase::AllocateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	// Removal only ever raises need_resize, so this is the one place it has to be folded in - and it
	// must happen before anything below reads the per-curve vectors.
	CompactRemoved();

	// Every curve owns exactly `resolution` samples, merged or not (see merge_with_previous), so the
	// layout never depends on merging.
	total_points = 0;
	for (size_t curveIndex = 0; curveIndex < TotalCurves(); ++curveIndex)
		total_points += resolutions[curveIndex];

	const auto curve_count = static_cast<uint32_t>(TotalCurves());
	if (total_points < 2u || curve_count == 0u) {
		total_points = 0;
		total_segments = 0;
		// UploadCurves, which normally recomputes it, is not reached on this path.
		pattern_upper_bound = 0;
		return;
	}

	// resolution - 1 segments per curve, and none for the step from one curve to the next.
	total_segments = total_points - curve_count;

	// Grow when full, shrink once a quarter or less is in use - see FitCapacity(). Shrinking goes
	// through exactly the same reset() as growing; every buffer below is rewritten from scratch by the
	// uploads at the end of this function or by the next point pass, so nothing has to be carried over.
	const uint32_t points_required = FitCapacity(points_allocated, total_points);
	const uint32_t curves_required = FitCapacity(curves_allocated, curve_count);

	if (points_allocated != points_required) {
		AllocatePointBuffers(device, points_required); // also creates the layout buffers this renderer reads
		points_allocated = points_required;
	}

	if (curves_allocated != curves_required) {
		curve_control_points.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT3X4>(curves_required)));
		curve_colors.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadColorData>(curves_required)));
		curve_indices.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadSampleRange>(curves_required)));
		AllocateStyleBuffer(device, curves_required);
		AllocateCurveBuffers(device, curves_required);
		curves_allocated = curves_required;
	}

	std::vector<uint32_t> index_map;
	std::vector<uint32_t> curve_begin_bits;
	std::vector<uint32_t> segment_entries;
	if (bezier_data_map) index_map.reserve(total_points);
	if (curve_begins) curve_begin_bits.assign(std::max((points_allocated + 31u) / 32u, 1u), 0u);
	if (segment_map) segment_entries.reserve(total_segments);

	uint32_t current = 0;
	for (uint32_t curveIndex = 0; curveIndex < curve_count; ++curveIndex) {
		const uint32_t resolution = resolutions[curveIndex];
		const bool chain_start = IsChainStart(curveIndex);

		// One begin bit per CHAIN, not per curve: the scan restarts only where a new stroke starts, so
		// a merged curve's arc lengths carry on from the curve before it. See IsChainStart().
		//
		// The begin bit and the segment flags are the ONLY things merging changes here. A merged
		// curve still owns all of its own samples, its first one being its own copy of the joint (its
		// t = 0, the same point as the previous curve's t = 1 by the shared-endpoint assumption). The
		// point pass measures a zero chord from the previous curve's last sample to it, so the arc
		// length runs on unbroken.
		if (curve_begins && chain_start)
			curve_begin_bits[current / 32u] |= (1u << (current % 32u));

		if (bezier_data_map)
			index_map.insert(index_map.end(), resolution, curveIndex);

		// The curve's resolution - 1 segments, and none for the step to the next curve, which is how the
		// draws come to have no bridge in them. Inside the curve the stroke always goes on; at its two
		// ends only into a merged neighbour.
		if (segment_map) {
			const bool joined_after = curveIndex + 1u < curve_count && !IsChainStart(curveIndex + 1u);
			for (uint32_t j = 0; j + 1u < resolution; ++j)
				segment_entries.push_back(curveIndex
					| ((j > 0u || !chain_start) ? SegmentHasPrev : 0u)
					| ((j + 2u < resolution || joined_after) ? SegmentHasNext : 0u));
		}

		current += resolution;
	}
	assert(current == total_points);
	assert(!segment_map || segment_entries.size() == total_segments);

	if (bezier_data_map) bezier_data_map->Upload(std::span<const uint32_t>{ index_map }, context);
	if (curve_begins) curve_begins->Upload(std::span<const uint32_t>{ curve_begin_bits }, context);
	if (segment_map) segment_map->Upload(std::span<const uint32_t>{ segment_entries }, context);

	// Everything: a buffer that was just (re)allocated holds nothing yet.
	UploadCurves(context, DirtyAll);
}

bool BezierRendererBase::UpdateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	// CPU only: this is buffer allocation and the staging uploads, the GPU side of it is a copy the
	// driver folds into the next pass. On a frame that changes nothing this measures the two bool
	// tests below and nothing else, which is what makes a spike here easy to spot.
	profiler.begin_cpu("update");

	bool updated = false;
	if (need_resize) {
		AllocateBuffers(device, context);
		need_resize = false;
		need_upload = false;
		upload_parts = 0;
		updated = true;
	}
	else if (need_upload) {
		// A need_upload with no part recorded can only come from code that sets the flag directly;
		// treat it as "anything may have changed".
		UploadCurves(context, upload_parts ? upload_parts : static_cast<uint8_t>(DirtyAll));
		need_upload = false;
		upload_parts = 0;
		updated = true;
	}

	profiler.end_cpu("update");
	return updated;
}

void BezierRendererBase::UploadCameraData(const XMMATRIX& view_proj, GraphicsDeviceContext* context) {
	XMStoreFloat4x4(&camera_cb_data.VP, XMMatrixTranspose(view_proj));
	camera_cb_data.TotalPointCount = total_points;
	camera_cb_data.TotalCurveCount = static_cast<uint32_t>(TotalCurves());
	viewport_data->Upload(camera_cb_data, context);
}

void BezierRendererBase::UpdatePatternBound() {
	const auto start = std::chrono::steady_clock::now();
	// cubic_points and spacings are parallel vectors (one entry per curve), so the two-range overload
	// pairs them up without an index. par_unseq: each curve is independent and the sum is
	// associative, so the work can be split across threads and vectorised in any order.
	const uint64_t bound = std::transform_reduce(std::execution::par_unseq,
		cubic_points.begin(), cubic_points.end(), spacings.begin(), uint64_t{ 0 },
		std::plus<>{},
		[](const XMFLOAT3X4& cubic, float spacing) {
			if (!(spacing > 0.f)) return static_cast<uint64_t>(0);

			XMVECTOR x = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(cubic.m[0]));
			XMVECTOR y = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(cubic.m[1]));
			XMVECTOR z = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(cubic.m[2]));
			XMVECTOR dx = XMVectorSubtract(XMVectorSwizzle<1, 2, 3, 3>(x), x);
			XMVECTOR dy = XMVectorSubtract(XMVectorSwizzle<1, 2, 3, 3>(y), y);
			XMVECTOR dz = XMVectorSubtract(XMVectorSwizzle<1, 2, 3, 3>(z), z);
			XMVECTOR edges = XMVectorSqrt(XMVectorMultiplyAdd(dz, dz, XMVectorMultiplyAdd(dy, dy, XMVectorMultiply(dx, dx))));
			float polygon = XMVectorGetX(XMVector4Dot(edges, XMVectorSplatOne()));

			const float centers = std::floor(polygon / spacing) + 1.f;
			constexpr float cap = static_cast<float>(maxPatternCount);
			return static_cast<uint64_t>(centers < cap ? centers : cap);
		});
	const auto elapsed = std::chrono::steady_clock::now() - start;

	if (!owners.empty()) {
		const double ns_per_curve = std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(owners.size());
		pattern_bound_min_ns = std::min(pattern_bound_min_ns, ns_per_curve);
	}
	pattern_upper_bound = static_cast<uint32_t>(std::min<uint64_t>(bound, maxPatternCount));
}

// Whether `prev` ends where `next` starts: P3 of `prev` (its last column) against P0 of `next` (its
// first column) - see cubic_points.
bool EndpointsMeet(const XMFLOAT3X4& prev, const XMFLOAT3X4& next) {
	const XMVECTOR va = XMVectorSet(prev.m[0][3], prev.m[1][3], prev.m[2][3], 0.f);
	const XMVECTOR vb = XMVectorSet(next.m[0][0], next.m[1][0], next.m[2][0], 0.f);
	const float scale = std::max(1.0f, XMVectorGetX(XMVectorMax(XMVector3Length(va), XMVector3Length(vb))));
	return XMVectorGetX(XMVector3Length(va - vb)) <= 1e-4f * scale;
}

void BezierRendererBase::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	if (owners.empty() || !curve_control_points) return;

	// The vectors already hold the GPU form (cubic points, packed colours), so these are plain copies.
	if (parts & DirtyPositions) {
#ifndef NDEBUG
		// Merging assumes the two curves meet: the segment between the previous curve's last sample
		// and this curve's first is taken to be zero-length and is never drawn, and the joins on both
		// sides of it are built as if the stroke ran on unbroken, so a gap would leave a hole. The
		// tolerance is relative, so it holds at any scene scale.
		for (size_t curveIndex = 1; curveIndex < owners.size(); ++curveIndex)
			assert((IsChainStart(curveIndex) || EndpointsMeet(cubic_points[curveIndex - 1u], cubic_points[curveIndex])) &&
				"merge_with_previous on a curve that does not start where the previous one ends");
#endif
		// CPU time of the Map(WRITE_DISCARD) + copy + Unmap; the GPU reads the data when a pass uses it,
		// so there is no separate GPU copy to time. Only recorded on frames that upload, so the
		// average is over uploads, not over frames.
		profiler.begin_cpu("upload points");
		curve_control_points->Upload(std::span<const XMFLOAT3X4>{ cubic_points }, context);
		profiler.end_cpu("upload points");
	}

	if (parts & DirtyColors) {
		profiler.begin_cpu("upload colors");
		curve_colors->Upload(std::span<const UploadColorData>{ color_data }, context);
		profiler.end_cpu("upload colors");
	}

	// Disjoint and back to back, merged or not: a curve's first sample is the one right after the
	// previous curve's last (see BezierData::merge_with_previous).
	if (parts & DirtyLayout) {
		index_scratch.resize(owners.size());
		uint32_t current = 0;
		for (size_t curveIndex = 0; curveIndex < owners.size(); ++curveIndex) {
			const uint32_t first = current;
			const uint32_t last = first + resolutions[curveIndex] - 1u;
			index_scratch[curveIndex] = UploadSampleRange{ first, last };
			current = last + 1u;
		}
		assert(current == total_points);
		curve_indices->Upload(std::span<const UploadSampleRange>{ index_scratch }, context);
	}

	// The renderer's own per-curve style (the line renderer has none - its UploadStyles is empty).
	if (parts & DirtyStyles)
		UploadStyles(context);
}

void BezierRendererBase::BindCurveData(GraphicsDeviceContext* context,
	uint32_t control_point_slot, uint32_t color_slot, uint32_t index_slot) {
	curve_control_points->Bind(ShaderStage::Vertex, control_point_slot, context);
	curve_colors->Bind(ShaderStage::Vertex, color_slot, context);
	curve_indices->Bind(ShaderStage::Vertex, index_slot, context);
}
