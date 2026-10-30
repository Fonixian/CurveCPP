#include "bezier_common.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

using namespace Axodox::Graphics;
using namespace DirectX;

const char* CurveKindName(CurveKind kind) {
	switch (kind) {
	case CurveKind::Solid:     return "Solid";
	case CurveKind::Patterned: return "Patterned";
	case CurveKind::Dotted:    return "Dots";
	}
	return "?";
}

static BezierData FromCubic(const XMFLOAT3 (&cubic)[4]) {
	BezierData result;
	result.P0 = cubic[0];
	result.P1 = cubic[1];
	result.P2 = cubic[2];
	result.P3 = cubic[3];
	return result;
}

BezierData Linear(XMFLOAT3 P0, XMFLOAT3 P1) {
	XMFLOAT3 cubic[4];
	LinearToCubic(P0, P1, cubic);
	return FromCubic(cubic);
}

BezierData Quadratic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2) {
	XMFLOAT3 cubic[4];
	QuadraticToCubic(P0, P1, P2, cubic);
	return FromCubic(cubic);
}

BezierData Cubic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2, XMFLOAT3 P3) {
	const XMFLOAT3 cubic[4] = { P0, P1, P2, P3 };
	return FromCubic(cubic);
}

unsigned next_pow2(unsigned x) { return x <= 1u ? 1u : 1u << (std::numeric_limits<unsigned>::digits - std::countl_zero(x - 1u)); }

uint32_t FitCapacity(uint32_t allocated, uint32_t required) {
	const uint32_t fitted = next_pow2(std::max(required, 1u));
	if (allocated < fitted) return fitted;
	if (fitted <= allocated / 4u) return fitted;
	return allocated;
}

uint32_t PackFloat3ToR8G8B8A8(const XMFLOAT3& color) {
	const float r = std::clamp(color.x, 0.0f, 1.0f);
	const float g = std::clamp(color.y, 0.0f, 1.0f);
	const float b = std::clamp(color.z, 0.0f, 1.0f);

	const uint32_t ir = static_cast<uint32_t>(r * 255.0f + 0.5f);
	const uint32_t ig = static_cast<uint32_t>(g * 255.0f + 0.5f);
	const uint32_t ib = static_cast<uint32_t>(b * 255.0f + 0.5f);
	const uint32_t ia = 255u;

	return (ia << 24) | (ib << 16) | (ig << 8) | ir;
}

XMFLOAT3 UnpackFloat3FromR8G8B8A8(uint32_t color) {
	return XMFLOAT3{
		float(color & 0xFFu) / 255.0f,
		float((color >> 8) & 0xFFu) / 255.0f,
		float((color >> 16) & 0xFFu) / 255.0f
	};
}

uint32_t PackWidthByte(float width) {
	return static_cast<uint32_t>(std::clamp(width, 0.f, 255.f) + 0.5f);
}

uint32_t PackWidthCapCapJoin(const BezierData& bez) {
	return (PackWidthByte(bez.width) << 24) |
		(uint32_t(bez.cap_front) << 16) |
		(uint32_t(bez.cap_back) << 8) |
		uint32_t(bez.join);
}

XMFLOAT3 LerpFloat3(const XMFLOAT3& a, const XMFLOAT3& b, float t) {
	XMFLOAT3 out;
	XMStoreFloat3(&out, XMVectorLerp(XMLoadFloat3(&a), XMLoadFloat3(&b), t));
	return out;
}

void LinearToCubic(const XMFLOAT3& p0, const XMFLOAT3& p1, XMFLOAT3 (&cubic)[4]) {
	cubic[0] = p0;
	cubic[1] = LerpFloat3(p0, p1, 1.0f / 3.0f);
	cubic[2] = LerpFloat3(p0, p1, 2.0f / 3.0f);
	cubic[3] = p1;
}

void QuadraticToCubic(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, XMFLOAT3 (&cubic)[4]) {
	cubic[0] = p0;
	cubic[1] = LerpFloat3(p0, p1, 2.0f / 3.0f);
	cubic[2] = LerpFloat3(p1, p2, 1.0f / 3.0f);
	cubic[3] = p2;
}

constexpr uint32_t computeSrvSlots = 5u;
constexpr uint32_t computeUavSlots = 5u;
void ClearComputeBindings(GraphicsDeviceContext* context) {
	for (uint32_t slot = 0; slot < computeSrvSlots; ++slot)
		context->BindShaderResourceView(nullptr, ShaderStage::Compute, slot);
	for (uint32_t slot = 0; slot < computeUavSlots; ++slot)
		context->BindUnorderedAccessView(nullptr, slot);
}

// The patterned renderer's curve_vs binds t0..t8.
constexpr uint32_t vertexSrvSlots = 9u;
// Only t1 (pattern positions) is ever bound to the pixel stage.
constexpr uint32_t pixelSrvSlots = 2u;
void ClearDrawBindings(GraphicsDeviceContext* context) {
	for (uint32_t slot = 0; slot < vertexSrvSlots; ++slot)
		context->BindShaderResourceView(nullptr, ShaderStage::Vertex, slot);
	for (uint32_t slot = 1; slot < pixelSrvSlots; ++slot)
		context->BindShaderResourceView(nullptr, ShaderStage::Pixel, slot);
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

void BezierRendererBase::Remove(BezierCurve& curve) {
	if (curve.renderer == this) curve.Remove();
}

// Stable, so chains survive. A curve right after a removed one becomes a chain start.
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

void BezierRendererBase::MoveCurve(size_t index, BezierRendererBase& target, bool chain_start) {
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

// --- BezierCurve ----------------------------------------------------------------------------------

BezierCurve::BezierCurve(BezierCurve&& other) noexcept
	: curve_index(other.curve_index), renderer(other.renderer) {
	if (renderer) renderer->owners[curve_index] = this;
	other.renderer = nullptr;
}

BezierCurve& BezierCurve::operator=(BezierCurve&& other) noexcept {
	if (this == &other) return *this;

	Remove();

	curve_index = other.curve_index;
	renderer = other.renderer;
	if (renderer) renderer->owners[curve_index] = this;
	other.renderer = nullptr;
	return *this;
}

void BezierCurve::Remove() {
	if (!renderer) return;

	assert(renderer->owners[curve_index] == this);
	renderer->owners[curve_index] = nullptr;
	++renderer->removed_count;
	renderer->need_resize = true;
	// The next curve may start a chain of its own now.
	if (curve_index + 1 < renderer->owners.size()) renderer->MarkForRouting(curve_index + 1);
	renderer = nullptr;
}

uint64_t PatternCenterBound(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3, float spacing) {
	if (!(spacing > 0.f)) return 0;

	const float polygon =
		XMVectorGetX(XMVector3Length(XMLoadFloat3(&p1) - XMLoadFloat3(&p0))) +
		XMVectorGetX(XMVector3Length(XMLoadFloat3(&p2) - XMLoadFloat3(&p1))) +
		XMVectorGetX(XMVector3Length(XMLoadFloat3(&p3) - XMLoadFloat3(&p2)));

	const double centers = std::floor(static_cast<double>(polygon) / static_cast<double>(spacing)) + 1.0;
	return static_cast<uint64_t>(std::clamp(centers, 0.0, static_cast<double>(maxPatternCount)));
}

bool EndpointsMeet(const XMFLOAT3& a, const XMFLOAT3& b) {
	const XMVECTOR va = XMLoadFloat3(&a), vb = XMLoadFloat3(&b);
	const float scale = std::max(1.0f, XMVectorGetX(XMVectorMax(XMVector3Length(va), XMVector3Length(vb))));
	return XMVectorGetX(XMVector3Length(va - vb)) <= 1e-4f * scale;
}

bool BezierRendererBase::IsChainStart(size_t index) const {
	return index == 0 || merged[index] == 0;
}

void BezierRendererBase::AllocateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	CompactRemoved();

	// A merged curve shares its first sample with the previous curve.
	total_points = 0;
	for (size_t curveIndex = 0; curveIndex < TotalCurves(); ++curveIndex)
		total_points += resolutions[curveIndex] - (IsChainStart(curveIndex) ? 0u : 1u);

	const auto curve_count = static_cast<uint32_t>(TotalCurves());
	if (total_points < 2u || curve_count == 0u) {
		total_points = 0;
		pattern_upper_bound = 0;
		return;
	}

	const uint32_t points_required = FitCapacity(points_allocated, total_points);
	const uint32_t curves_required = FitCapacity(curves_allocated, curve_count);

	if (points_allocated != points_required) {
		curve_begins.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::max((points_required + 31u) / 32u, 1u))));
		bezier_data_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(points_required)));
		AllocatePointBuffers(device, points_required);
		points_allocated = points_required;
	}

	if (curves_allocated != curves_required) {
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
	for (uint32_t curveIndex = 0; curveIndex < curve_count; ++curveIndex) {
		const uint32_t resolution = resolutions[curveIndex];

		// One begin bit per chain, so arc lengths carry on across merged curves.
		if (IsChainStart(curveIndex)) {
			curve_begin_bits[current / 32u] |= (1u << (current % 32u));
			for (unsigned i = 0; i < resolution; ++i)
				index_map.push_back(curveIndex);
			current += resolution;
		}
		else {
			// The joint sample belongs to the curve that starts there; the previous one still ends on it.
			index_map.back() = curveIndex;
			for (unsigned i = 1; i < resolution; ++i)
				index_map.push_back(curveIndex);
			current += resolution - 1u;
		}
	}
	assert(current == total_points);

	bezier_data_map->Upload(std::span<const uint32_t>{ index_map }, context);
	curve_begins->Upload(std::span<const uint32_t>{ curve_begin_bits }, context);

	UploadCurves(context, DirtyAll);
}

void BezierRendererBase::MarkForRouting(size_t index) {
	if (route_all || (!route_marks.empty() && route_marks.back() == index)) return;
	if (route_marks.size() >= owners.size()) {
		route_all = true;
		route_marks.clear();
		return;
	}
	route_marks.push_back(static_cast<uint32_t>(index));
}

bool BezierRendererBase::UpdateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	profiler.begin_cpu("update");

	// BezierRenderer has routed by now; a compaction below would make the indices stale.
	route_marks.clear();
	route_all = false;

	bool updated = false;
	if (need_resize) {
		AllocateBuffers(device, context);
		need_resize = false;
		need_upload = false;
		upload_parts = 0;
		updated = true;
	}
	else if (need_upload) {
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
	uint64_t bound = 0;
	for (size_t curveIndex = 0; curveIndex < owners.size(); ++curveIndex) {
		const XMFLOAT3* p = &cubic_points[curveIndex * 4u];
		bound += PatternCenterBound(p[0], p[1], p[2], p[3], spacings[curveIndex]);
	}
	pattern_upper_bound = static_cast<uint32_t>(std::min<uint64_t>(bound, maxPatternCount));
}

void BezierRendererBase::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	if (owners.empty() || !curve_control_points) return;

	if (parts & DirtyPositions) {
#ifndef NDEBUG
		// Only the later curve's P0 is evaluated at a joint, so a gap would be closed silently.
		for (size_t curveIndex = 1; curveIndex < owners.size(); ++curveIndex)
			assert((IsChainStart(curveIndex) || EndpointsMeet(cubic_points[curveIndex * 4u - 1u], cubic_points[curveIndex * 4u])) &&
				"merge_with_previous on a curve that does not start where the previous one ends");
#endif
		curve_control_points->Upload(std::span<const XMFLOAT3>{ cubic_points }, context);
	}

	if (parts & DirtyColors)
		curve_colors->Upload(std::span<const UploadColorData>{ color_data }, context);

	if (parts & DirtyLayout) {
		index_scratch.resize(owners.size());
		uint32_t current = 0;
		for (size_t curveIndex = 0; curveIndex < owners.size(); ++curveIndex) {
			const uint32_t first = IsChainStart(curveIndex) ? current : current - 1u;
			const uint32_t last = first + resolutions[curveIndex] - 1u;
			index_scratch[curveIndex] = UploadSampleRange{ first, last };
			current = last + 1u;
		}
		assert(current == total_points);
		curve_indices->Upload(std::span<const UploadSampleRange>{ index_scratch }, context);
	}

	if (parts & DirtyStyles)
		UploadStyles(context);
}

void BezierRendererBase::BindCurveData(GraphicsDeviceContext* context,
	uint32_t control_point_slot, uint32_t color_slot, uint32_t index_slot) {
	curve_control_points->Bind(ShaderStage::Vertex, control_point_slot, context);
	curve_colors->Bind(ShaderStage::Vertex, color_slot, context);
	curve_indices->Bind(ShaderStage::Vertex, index_slot, context);
}
