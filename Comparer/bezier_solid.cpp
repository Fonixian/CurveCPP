#include "bezier_solid.h"
#include <DirectXPackedVector.h>
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

BezierSolidRenderer::BezierSolidRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device) {
	curve_draw.vs = Pipeline::getVS(device, "solid_vert.cso");
	curve_draw.ps = Pipeline::getPS(device, "solid_ps.cso");
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});
}

void BezierSolidRenderer::AllocateCurveData(const GraphicsDevice& device, uint32_t curves_required) {
	bezier_data.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadSolidCurveData>(curves_required)));
	control_points.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT3>(curves_required * 4u)));
}

// Two halves in one word: min in the low 16 bits, max in the high 16. See UploadSolidCurveData.
static uint32_t PackHeightRange(float min_height, float max_height) {
	const uint32_t lo = PackedVector::XMConvertFloatToHalf(min_height);
	const uint32_t hi = PackedVector::XMConvertFloatToHalf(max_height);
	return (hi << 16) | lo;
}

void BezierSolidRenderer::UploadCurveData(GraphicsDeviceContext* context) {
	// No pattern buffer in this renderer, so nothing to bound.
	pattern_upper_bound = 0;

	if (curves.empty() || !bezier_data || !control_points) return;

	std::vector<UploadSolidCurveData> curve_data;
	curve_data.reserve(curves.size());
	std::vector<XMFLOAT3> points;
	points.reserve(curves.size() * 4u);

	uint32_t current = 0; // next sample index
	[[maybe_unused]] XMFLOAT3 previous_end = {};
	for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
		const BezierData& bez = curves[curveIndex];
		const bool chain_start = IsChainStart(curveIndex);

		// The control points as given: power() + 1 of them, no degree elevation.
		const XMFLOAT3 source[4] = { bez.P0, bez.P1, bez.P2, bez.P3 };
		const uint32_t count = static_cast<uint32_t>(std::clamp(bez.power(), 1, 3)) + 1u;

		// Same shared-endpoint assumption as the base UploadCurveData: only this curve's P0 is ever
		// evaluated at a merged joint.
		assert((chain_start || EndpointsMeet(previous_end, source[0])) &&
			"merge_with_previous on a curve that does not start where the previous one ends");
		previous_end = source[count - 1u];

		const uint32_t control_first = static_cast<uint32_t>(points.size());
		points.insert(points.end(), source, source + count);

		// A merged curve starts ON the previous curve's last sample - see AllocateBuffers.
		const uint32_t first = chain_start ? current : current - 1u;

		const uint32_t count_caps_join =
			(count << 24) |
			(uint32_t(bez.cap_front) << 16) |
			(uint32_t(bez.cap_back) << 8) |
			uint32_t(bez.join);

		curve_data.push_back(UploadSolidCurveData{
			control_first,
			first,
			bez.resolution,
			count_caps_join,
			PackFloat3ToR8G8B8A8(bez.C0),
			PackFloat3ToR8G8B8A8(bez.C1),
			PackHeightRange(bez.min_height, bez.max_height),
			bez.width
		});

		current = first + bez.resolution;
	}
	assert(current == total_points);

	// Both are plain StructuredBuffers (Map / WRITE_DISCARD), so uploading only the live prefix of an
	// over-allocated buffer is safe.
	bezier_data->Upload(std::span<const UploadSolidCurveData>{ curve_data }, context);
	control_points->Upload(std::span<const XMFLOAT3>{ points }, context);
}

void BezierSolidRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !bezier_data || !control_points) {
		EndDraw();
		return;
	}

	UploadCameraData(view_proj, context);

	// No compute work at all: no "calc", "scan" or "pattern" metric, so those rows stay empty in the
	// profiler window. solid_vert evaluates the curve per vertex instead of reading a point pass's
	// output, so what the calc pass used to cost is now folded into "draw".
	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	// Same slot numbers as curve_vs where the meaning is shared. t0 held the calculated points in the
	// patterned renderer; here it holds the control points they are evaluated from. t2, t5 and t6 are
	// unused: no distances, no pattern, and the style is part of the curve record at t3.
	control_points->Bind(ShaderStage::Vertex, 0, context);   // t0: control points, 2-4 per curve
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);
	bezier_data->Bind(ShaderStage::Vertex, 3, context);      // t3: UploadSolidCurveData
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context);
	viewport_data->Bind(ShaderStage::Pixel, 1, context);

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_points - 1u, 0, 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
