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

uint32_t FitCapacity(uint32_t allocated, uint32_t required) {
	const uint32_t fitted = next_pow2(std::max(required, 1u));
	if (allocated < fitted) return fitted;          // does not fit: grow
	if (fitted <= allocated / 4u) return fitted;    // a quarter or less in use: shrink to fit
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

// Bernstein -> monomial, so the point passes can evaluate by Horner. Expanding
//
//     P(t) = (1-t)^3 P0 + 3(1-t)^2 t P1 + 3(1-t) t^2 P2 + t^3 P3
//
// and collecting powers of t gives K0 = P0, K1 = 3(P1 - P0), K2 = 3(P0 - 2P1 + P2),
// K3 = -P0 + 3P1 - 3P2 + P3 - which are, up to the binomial factors, the forward differences of the
// control polygon. Taking the differences FIRST and combining them second (rather than scaling the
// control points and summing) keeps the cancellation local: K3 is a third difference either way, but
// this spelling never forms 3*P1 and 3*P2 as separate large intermediates.
//
// Endpoints: K0 == P0 exactly, so P(0) is bit-exact. P(1) = K0+K1+K2+K3 reaches P3 only in exact
// arithmetic - measured at 4.5e-6 world units worst case over 20k random curves at scene scale, and
// chord lengths of drawable size (>= 1e-2 world units) agree with the Bernstein form to 1e-4
// relative, total arc length to 2e-6.
//
// The one behavioural difference, and it is not a rounding curiosity: on a curve whose control points
// are EXACTLY duplicated this form is exactly constant (K1 = K2 = K3 = 0) where the Bernstein form
// wobbled by an ulp, so adjacent samples now land on the same float. That is what the vertex shaders
// want - a zero-length segment gets l_AB == 0 -> 0/0 -> NaN -> culled, which is the behaviour the "do
// NOT drop the +0.5 in the NDC->screen map" note in solid_vert.hlsl is protecting. The reverse
// happens too, in 34 of 618k sample pairs, all with duplicated control points: a pair that used to be
// bit-identical is now a hair apart. 22 of those still collapse to the same float once projected; the
// other 12 separate by at most 8e-5 px, which is the "misoriented join instead of nothing" case that
// note describes. If that ever shows up, the fix is to compare l_AB against a small epsilon in the
// vertex shaders instead of relying on an exact zero - the old code was equally exposed to it.
void ToPowerBasis(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3,
	XMFLOAT3& k0, XMFLOAT3& k1, XMFLOAT3& k2, XMFLOAT3& k3) {
	const XMVECTOR v0 = XMLoadFloat3(&p0);
	const XMVECTOR v1 = XMLoadFloat3(&p1);
	const XMVECTOR v2 = XMLoadFloat3(&p2);
	const XMVECTOR v3 = XMLoadFloat3(&p3);

	const XMVECTOR three = XMVectorReplicate(3.0f);

	const XMVECTOR d10 = v1 - v0;   // first differences of the control polygon
	const XMVECTOR d21 = v2 - v1;
	const XMVECTOR d32 = v3 - v2;

	const XMVECTOR dd0 = d21 - d10; // second differences
	const XMVECTOR dd1 = d32 - d21;

	XMStoreFloat3(&k0, v0);
	XMStoreFloat3(&k1, three * d10);
	XMStoreFloat3(&k2, three * dd0);
	XMStoreFloat3(&k3, dd1 - dd0); // the third difference
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
// t1 pattern positions, t2 pattern offsets. Slot 0 is never bound to the pixel stage by any of the
// three renderers, so the loop starts at 1.
constexpr uint32_t pixelSrvSlots = 3u;
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
	assert(curve.power() >= 1);

	const size_t index = curves.size();
	curves.push_back(curve);
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
	for (size_t read = 0; read < curves.size(); ++read) {
		BezierCurve* owner = owners[read];
		if (!owner) {
			previous_removed = true;
			continue;
		}

		if (previous_removed) curves[read].merge_with_previous = false;
		previous_removed = false;

		if (write != read) {
			curves[write] = curves[read];
			owners[write] = owner;
		}
		owner->curve_index = write;
		++write;
	}

	curves.resize(write);
	owners.resize(write);
	removed_count = 0;
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
	renderer = nullptr;
}

bool EndpointsMeet(const XMFLOAT3& a, const XMFLOAT3& b) {
	const XMVECTOR va = XMLoadFloat3(&a), vb = XMLoadFloat3(&b);
	const float scale = std::max(1.0f, XMVectorGetX(XMVectorMax(XMVector3Length(va), XMVector3Length(vb))));
	return XMVectorGetX(XMVector3Length(va - vb)) <= 1e-4f * scale;
}

bool BezierRendererBase::IsChainStart(size_t index) const {
	return index == 0 || !curves[index].merge_with_previous;
}

void BezierRendererBase::AllocateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	// Removal only ever raises need_resize, so this is the one place it has to be folded in - and it
	// must happen before anything below reads `curves`.
	CompactRemoved();

	// A merged curve shares its first sample with the previous curve's last one (see
	// merge_with_previous), so it adds one sample fewer than its resolution.
	total_points = 0;
	for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex)
		total_points += curves[curveIndex].resolution - (IsChainStart(curveIndex) ? 0u : 1u);

	const auto curve_count = static_cast<uint32_t>(curves.size());
	if (total_points < 2u || curve_count == 0u) {
		total_points = 0;
		// UploadCurveData, which normally recomputes it, is not reached on this path.
		pattern_upper_bound = 0;
		return;
	}

	// Grow when full, shrink once a quarter or less is in use - see FitCapacity(). Shrinking goes
	// through exactly the same reset() as growing; every buffer below is rewritten from scratch by the
	// uploads at the end of this function or by the next point pass, so nothing has to be carried over.
	const uint32_t points_required = FitCapacity(points_allocated, total_points);
	const uint32_t curves_required = FitCapacity(curves_allocated, curve_count);

	if (points_allocated != points_required) {
		if (NeedsCalculatedPoints())
			calculated_points.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT4>(points_required)));
		curve_begins.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::max((points_required + 31u) / 32u, 1u))));
		bezier_data_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(points_required)));
		AllocatePointBuffers(device, points_required);
		points_allocated = points_required;
	}

	if (curves_allocated != curves_required) {
		AllocateCurveData(device, curves_required);
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

		// One begin bit per CHAIN, not per curve: the scan restarts only where a new stroke starts, so
		// a merged curve's arc lengths carry on from the curve before it. See IsChainStart().
		if (IsChainStart(curveIndex)) {
			curve_begin_bits[current / 32u] |= (1u << (current % 32u));
			for (unsigned i = 0; i < bez.resolution; ++i)
				index_map.push_back(curveIndex);
			current += bez.resolution;
		}
		else {
			// The joint sample is the previous curve's last one. It is handed to THIS curve, because a
			// sample's owner is the curve whose interval starts there: the point pass evaluates it as
			// this curve's t = 0 (the same point, by the shared-endpoint assumption) and measures the
			// chord to this curve's next sample, and the vertex shaders style the segment leaving it
			// with this curve's width and caps. The previous curve still ENDS on it - its LastIndex
			// is unchanged - it just no longer evaluates it.
			index_map.back() = curveIndex;
			for (unsigned i = 1; i < bez.resolution; ++i)
				index_map.push_back(curveIndex);
			current += bez.resolution - 1u;
		}
	}
	assert(current == total_points);

	bezier_data_map->Upload(std::span<const uint32_t>{ index_map }, context);
	curve_begins->Upload(std::span<const uint32_t>{ curve_begin_bits }, context);

	UploadCurveData(context);
}

void BezierRendererBase::AllocateCurveData(const GraphicsDevice& device, uint32_t curves_required) {
	bezier_data.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadBezierData>(curves_required)));
}

void BezierRendererBase::UploadCurveData(GraphicsDeviceContext* context) {
	pattern_upper_bound = 0;

	if (curves.empty() || !bezier_data || !curve_styles) return;

	std::vector<UploadBezierData> upload_data;
	upload_data.reserve(curves.size());

	// Chain ranges, in sample indices. The first sample of a chain is known on the way forward; the
	// last one only once the chain has ended, so it is patched in when the NEXT chain starts (and once
	// more after the loop for the final chain).
	size_t chain_first_curve = 0;
	int32_t chain_first_sample = 0;
	auto close_chain = [&](size_t end_curve) {
		if (upload_data.empty()) return;
		const int32_t chain_last_sample = upload_data.back().last_index;
		for (size_t k = chain_first_curve; k < end_curve; ++k)
			upload_data[k].chain_last_index = chain_last_sample;
	};

	uint64_t bound = 0;

	int32_t current = 0;
	[[maybe_unused]] XMFLOAT3 previous_end = {}; // P3 of the curve before, for the shared-endpoint check
	for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
		const BezierData& bez = curves[curveIndex];
		const bool chain_start = IsChainStart(curveIndex);

		if (chain_start) {
			close_chain(curveIndex);
			chain_first_curve = curveIndex;
			chain_first_sample = current;
		}

		XMFLOAT3 p0, p1, p2, p3;
		ToCubic(bez, p0, p1, p2, p3);

		// Merging assumes the two curves meet: only this curve's P0 is ever evaluated at the joint,
		// so a gap would be closed silently by dropping the previous curve's real endpoint. The
		// tolerance is relative, so it holds at any scene scale.
		assert((chain_start || EndpointsMeet(previous_end, p0)) &&
			"merge_with_previous on a curve that does not start where the previous one ends");
		previous_end = p3;

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

		// A merged curve starts ON the previous curve's last sample - see AllocateBuffers.
		const int32_t first = chain_start ? current : current - 1;
		const int32_t last = first + static_cast<int32_t>(bez.resolution) - 1;

		// Converted AFTER the bound above, which needs the control polygon itself. The shaders get the
		// monomial coefficients - see UploadBezierData in bezier_common.h.
		XMFLOAT3 k0, k1, k2, k3;
		ToPowerBasis(p0, p1, p2, p3, k0, k1, k2, k3);

		upload_data.push_back(UploadBezierData{
			k0, first,
			k1, last,
			k2, PackFloat3ToR8G8B8A8(bez.C0),
			k3, PackFloat3ToR8G8B8A8(bez.C1),
			bez.min_height, bez.max_height,
			chain_first_sample, last // chain_last_index is patched by close_chain()
		});

		current = last + 1;
	}
	close_chain(curves.size());

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
