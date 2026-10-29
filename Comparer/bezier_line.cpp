#include "bezier_line.h"

using namespace Axodox::Graphics;
using namespace DirectX;

BezierLineRenderer::BezierLineRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device) {
	curve_draw.vs = Pipeline::getVS(device, "line_vert.cso");
	curve_draw.ps = Pipeline::getPS(device, "line_ps.cso");
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});
}

void BezierLineRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !HasCurveData()) {
		EndDraw();
		return;
	}

	UploadCameraData(view_proj, context);

	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	BindCurveData(context, 3, 5, 6);                     // t3 control points, t5 colours, t6 indices
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context); // t4: curve per sample

	viewport_data->Bind(ShaderStage::Vertex, 1, context);

	// One line per sample (see bezier_data_map); the vertex shader culls the one starting on each
	// curve's last sample, so none of them bridges two curves.
	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
	context->get()->Draw(2u * total_points, 0);
	profiler.end_gpu("draw");

	UnbindShaderResources(context, ShaderStage::Vertex, { 3, 4, 5, 6 });
	EndDraw();
}
