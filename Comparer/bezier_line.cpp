#include "bezier_line.h"

using namespace Axodox::Graphics;
using namespace DirectX;

BezierLineRenderer::BezierLineRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device) {
	curve_draw.vs = Pipeline::getVS(device, "line_vert.cso");
	curve_draw.gs = Pipeline::getGS(device, "line_geom.cso");
	curve_draw.ps = Pipeline::getPS(device, "line_ps.cso");
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});
}

void BezierLineRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<float>(curves_required)));
}

void BezierLineRenderer::UploadStyles(GraphicsDeviceContext* context) {
	std::vector<float> widths;
	widths.reserve(curves.size());
	for (const auto& bez : curves)
		widths.push_back(bez.width);
	curve_styles->Upload(std::span<const float>{ widths }, context);
}

void BezierLineRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !bezier_data) {
		EndDraw();
		return;
	}

	UploadCameraData(view_proj, context);

	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);
	bezier_data->Bind(ShaderStage::Vertex, 3, context);
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);
	curve_styles->Bind(ShaderStage::Vertex, 6, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context);

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
	context->get()->Draw(total_points, 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
