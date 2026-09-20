#include "bezier_common.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

using namespace Axodox::Graphics;
using namespace DirectX;

bool BezierData::empty() const { return resolution == 0; }
void BezierData::clear() { resolution = 0; }
int BezierData::power() const { return bezier_power; }

BezierData Linear(XMFLOAT3 P0, XMFLOAT3 P1) {
	BezierData result;
	result.P0 = P0;
	result.P1 = P1;
	result.bezier_power = 1;
	return result;
}

BezierData Quadratic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2) {
	BezierData result;
	result.P0 = P0;
	result.P1 = P1;
	result.P2 = P2;
	result.bezier_power = 2;
	return result;
}

BezierData Cubic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2, XMFLOAT3 P3) {
	BezierData result;
	result.P0 = P0;
	result.P1 = P1;
	result.P2 = P2;
	result.P3 = P3;
	result.bezier_power = 3;
	return result;
}

unsigned next_pow2(unsigned x) { return x <= 1u ? 1u : 1u << (std::numeric_limits<unsigned>::digits - std::countl_zero(x - 1u)); }

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

XMFLOAT3 LerpFloat3(const XMFLOAT3& a, const XMFLOAT3& b, float t) {
	XMFLOAT3 out;
	XMStoreFloat3(&out, XMVectorLerp(XMLoadFloat3(&a), XMLoadFloat3(&b), t));
	return out;
}

void ToCubic(const BezierData& source, XMFLOAT3& p0, XMFLOAT3& p1, XMFLOAT3& p2, XMFLOAT3& p3) {
	switch (source.power()) {
	case 1:
		p0 = source.P0;
		p1 = LerpFloat3(source.P0, source.P1, 1.0f / 3.0f);
		p2 = LerpFloat3(source.P0, source.P1, 2.0f / 3.0f);
		p3 = source.P1;
		break;
	case 2:
		p0 = source.P0;
		p1 = LerpFloat3(source.P0, source.P1, 2.0f / 3.0f);
		p2 = LerpFloat3(source.P1, source.P2, 1.0f / 3.0f);
		p3 = source.P2;
		break;
	default:
		p0 = source.P0;
		p1 = source.P1;
		p2 = source.P2;
		p3 = source.P3;
		break;
	}
}

constexpr uint32_t computeSrvSlots = 5u;
constexpr uint32_t computeUavSlots = 5u;
void ClearComputeBindings(GraphicsDeviceContext* context) {
	for (uint32_t slot = 0; slot < computeSrvSlots; ++slot)
		context->BindShaderResourceView(nullptr, ShaderStage::Compute, slot);
	for (uint32_t slot = 0; slot < computeUavSlots; ++slot)
		context->BindUnorderedAccessView(nullptr, slot);
}

constexpr uint32_t vertexSrvSlots = 8u;
void ClearDrawBindings(GraphicsDeviceContext* context) {
	for (uint32_t slot = 0; slot < vertexSrvSlots; ++slot)
		context->BindShaderResourceView(nullptr, ShaderStage::Vertex, slot);
	context->BindShaderResourceView(nullptr, ShaderStage::Pixel, 1);
}


BezierRendererBase::BezierRendererBase(const GraphicsDevice& device) : profiler(device) {
	viewport_data = std::make_unique<ConstantBuffer>(device, camera_cb_data);
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
	assert(curve.power() >= 1);

	const size_t index = curves.size();
	curves.push_back(curve);
	need_resize = true;
	return BezierCurve{ this, index };
}

BezierCurve BezierRendererBase::At(size_t index) {
	assert(index < curves.size());
	return BezierCurve{ this, index };
}

void BezierRendererBase::AllocateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	total_points = 0;
	for (const auto& bez : curves) total_points += bez.resolution;

	const auto curve_count = static_cast<uint32_t>(curves.size());
	if (total_points < 2u || curve_count == 0u) {
		total_points = 0;
		// UploadCurveData, which normally recomputes it, is not reached on this path.
		pattern_upper_bound = 0;
		return;
	}

	const uint32_t points_required = next_pow2(total_points);
	const uint32_t curves_required = next_pow2(curve_count);

	if (points_allocated < points_required) {
		if (NeedsCalculatedPoints())
			calculated_points.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT4>(points_required)));
		curve_begins.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::max((points_required + 31u) / 32u, 1u))));
		bezier_data_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(points_required)));
		AllocatePointBuffers(device, points_required);
		points_allocated = points_required;
	}

	if (curves_allocated < curves_required) {
		bezier_data.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadBezierData>(curves_required)));
		AllocateStyleBuffer(device, curves_required);
		AllocateCurveBuffers(device, curves_required);
		curves_allocated = curves_required;
	}

	std::vector<uint32_t> index_map;
	index_map.reserve(total_points);

	std::vector<uint32_t> curve_begin_bits(std::max((points_allocated + 31u) / 32u, 1u), 0u);

	uint32_t current = 0;
	for (uint32_t curveIndex = 0; curveIndex < curve_count; ++curveIndex) {
		const BezierData& bez = curves[curveIndex];

		/*if (curveIndex == 0) */curve_begin_bits[current / 32u] |= (1u << (current % 32u));

		for (unsigned i = 0; i < bez.resolution; ++i)
			index_map.push_back(curveIndex);

		current += bez.resolution;
	}

	bezier_data_map->Upload(std::span<const uint32_t>{ index_map }, context);
	curve_begins->Upload(std::span<const uint32_t>{ curve_begin_bits }, context);

	UploadCurveData(context);
}

void BezierRendererBase::UploadCurveData(GraphicsDeviceContext* context) {
	pattern_upper_bound = 0;

	if (curves.empty() || !bezier_data || !curve_styles) return;

	std::vector<UploadBezierData> upload_data;
	upload_data.reserve(curves.size());

	uint64_t bound = 0;

	int32_t current = 0;
	for (const auto& bez : curves) {
		XMFLOAT3 p0, p1, p2, p3;
		ToCubic(bez, p0, p1, p2, p3);

		// The control polygon bounds the arc length, which bounds the chord sum the point pass
		// measures, so this can only ever over-count. See PatternBound() in bezier_common.h - this
		// loop already has the cubic form in hand, which is why the bound is computed here.
		if (bez.spacing > 0.f) {
			const float polygon =
				XMVectorGetX(XMVector3Length(XMLoadFloat3(&p1) - XMLoadFloat3(&p0))) +
				XMVectorGetX(XMVector3Length(XMLoadFloat3(&p2) - XMLoadFloat3(&p1))) +
				XMVectorGetX(XMVector3Length(XMLoadFloat3(&p3) - XMLoadFloat3(&p2)));

			const double centers = std::floor(static_cast<double>(polygon) / static_cast<double>(bez.spacing)) + 1.0;
			bound += static_cast<uint64_t>(std::clamp(centers, 0.0, static_cast<double>(maxPatternCount)));
		}

		const int32_t first = current;
		const int32_t last = current + static_cast<int32_t>(bez.resolution) - 1;

		upload_data.push_back(UploadBezierData{
			p0, first,
			p1, last,
			p2, PackFloat3ToR8G8B8A8(bez.C0),
			p3, PackFloat3ToR8G8B8A8(bez.C1),
			bez.min_height, bez.max_height,
			{ 0.0f, 0.0f }
		});

		current += static_cast<int32_t>(bez.resolution);
	}

	pattern_upper_bound = static_cast<uint32_t>(std::min<uint64_t>(bound, maxPatternCount));

	bezier_data->Upload(std::span<const UploadBezierData>{ upload_data }, context);

	UploadStyles(context);
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
		updated = true;
	}
	else if (need_upload) {
		UploadCurveData(context);
		need_upload = false;
		updated = true;
	}

	profiler.end_cpu("update");
	return updated;
}

void BezierRendererBase::UploadCameraData(const XMMATRIX& view_proj, GraphicsDeviceContext* context) {
	XMStoreFloat4x4(&camera_cb_data.VP, XMMatrixTranspose(view_proj));
	camera_cb_data.TotalPointCount = total_points;
	camera_cb_data.TotalCurveCount = static_cast<uint32_t>(curves.size());
	viewport_data->Upload(camera_cb_data, context);
}
