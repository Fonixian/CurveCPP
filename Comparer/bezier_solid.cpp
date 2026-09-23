#include "bezier_solid.h"

using namespace Axodox::Graphics;
using namespace DirectX;

struct UploadSolidStyle {
	float    width;
	uint32_t capcapjoin;
};

BezierSolidRenderer::BezierSolidRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device) {
	calc_points = Pipeline::getCS(device, "solid_calc_points.cso");
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
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadSolidStyle>(curves_required)));
}

void BezierSolidRenderer::UploadStyles(GraphicsDeviceContext* context) {
	std::vector<UploadSolidStyle> style_data;
	style_data.reserve(curves.size());

	for (const auto& bez : curves) {
		uint32_t capcapjoin = (uint32_t(bez.cap_front) << 16) |
							  (uint32_t(bez.cap_back) << 8) |
							  uint32_t(bez.join);
		style_data.push_back(UploadSolidStyle{
			bez.width,
			capcapjoin
		});
	}

	curve_styles->Upload(std::span<const UploadSolidStyle>{ style_data }, context);
}

void BezierSolidRenderer::RunPointPass(GraphicsDeviceContext* context) {
	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);
	bezier_data->Bind(ShaderStage::Compute, 0, context);
	bezier_data_map->Bind(ShaderStage::Compute, 1, context);
	calculated_points->BindUnordered(0, context);

	profiler.begin_gpu("calc");
	calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);
	profiler.end_gpu("calc");

	ClearComputeBindings(context);
}

void BezierSolidRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !calculated_points) {
		EndDraw();
		return;
	}

	UploadCameraData(view_proj, context);

	RunPointPass(context); // Doesnt need to run every frame

	// No "scan" or "pattern" metric here - this renderer has neither pass, so those rows stay empty
	// in the profiler window. That gap IS the cost of the pattern pipeline.
	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	calculated_points->BindOrdered(ShaderStage::Vertex, 0, context);
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);
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
