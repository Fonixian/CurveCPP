#include "bezier_solid.h"
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

// SolidCurveStyle in solid_common.hlsli: one packed uint per curve, see PackWidthCapCapJoin. A
// fractional width is drawn at the nearest whole pixel.
void BezierSolidRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required)));
}

void BezierSolidRenderer::UploadStyles(GraphicsDeviceContext* context) {
	style_scratch.resize(curves.size());
	for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex)
		style_scratch[curveIndex] = PackWidthCapCapJoin(curves[curveIndex]);
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
	// output, so there is no separate calc cost to report.
	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	// Same slot numbers as the patterned renderer's curve_vs for t1, t3..t7, so the two vertex shaders
	// stay easy to diff; t0, t2, t8 and t9 are the patterned renderer's alone and unbound here.
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
