#include "bezier_solid.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

// Matches SolidCurveStyle in solid_common.hlsli: one uint per curve,
//
//     width << 24 | cap_front << 16 | cap_back << 8 | join
//
// The low three bytes are the same CapCapJoin the other renderers pack, so FrontCap/BackCap/Join read
// it unchanged. The width takes the top byte, so it is rounded to a WHOLE pixel and clamped to
// [0, 255] - a fractional width is drawn at the nearest integer one.
static uint32_t PackSolidStyle(const BezierData& bez) {
	const uint32_t width = static_cast<uint32_t>(std::clamp(bez.width, 0.f, 255.f) + 0.5f);
	return (width << 24) |
		(uint32_t(bez.cap_front) << 16) |
		(uint32_t(bez.cap_back) << 8) |
		uint32_t(bez.join);
}

BezierSolidRenderer::BezierSolidRenderer(const GraphicsDevice& device)
	: BezierSplitRendererBase(device) {
	curve_draw.vs = Pipeline::getVS(device, "solid_vert.cso");
	curve_draw.ps = Pipeline::getPS(device, "solid_ps.cso");
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});
}

void BezierSolidRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required)));
}

void BezierSolidRenderer::UploadStyles(GraphicsDeviceContext* context) {
	style_scratch.resize(curves.size());
	for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex)
		style_scratch[curveIndex] = PackSolidStyle(curves[curveIndex]);
	curve_styles->Upload(std::span<const uint32_t>{ style_scratch }, context);
}

void BezierSolidRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !HasCurveData()) {
		EndDraw();
		return;
	}

	UploadCameraData(view_proj, context);

	// No compute work at all: no "calc", "scan" or "pattern" metric, so those rows stay empty in the
	// profiler window. solid_vert evaluates the curve per vertex instead of reading a point pass's
	// output, so what the calc pass used to cost is now folded into "draw".
	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	// t0 (calculated points) and t2 (distances) are the patterned renderer's and unused here. t1, t3,
	// t4, t6 keep curve_vs's slot numbers; the colours take t5 (pattern ranges there) and the indices
	// the free t7.
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);
	BindCurveData(context, 3, 5, 7);                         // t3 control points, t5 colours, t7 indices
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);
	curve_styles->Bind(ShaderStage::Vertex, 6, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context);
	viewport_data->Bind(ShaderStage::Pixel, 1, context);

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_points - 1u, 0, 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
