#include "bezier_solid.h"

using namespace Axodox::Graphics;
using namespace DirectX;

struct UploadSolidStyle {
	float    width;
	uint32_t capcapjoin;
};

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

void BezierSolidRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !bezier_data) {
		EndDraw();
		return;
	}

	UploadCameraData(view_proj, context);

	// No compute work at all: no "calc", "scan" or "pattern" metric, so those rows stay empty in the
	// profiler window. solid_vert evaluates the curve per vertex instead of reading a point pass's
	// output, so what the calc pass used to cost is now folded into "draw".
	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	// t0 (calculated points) is gone; t2 and t5 were never used here. Same slot numbers as curve_vs.
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);
	bezier_data->Bind(ShaderStage::Vertex, 3, context);      // t3: curve definitions, evaluated per vertex
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
