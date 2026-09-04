#include "bezier_solid.h"

using namespace Axodox::Graphics;
using namespace DirectX;

struct UploadSolidStyle {
	float    width;
	uint32_t capcapjoin;
	float    padding[2];
};

// --- BezierSolidRenderer -----------------------------------------------------------------------

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
							  uint32_t(CurveJoin::Round);
		style_data.push_back(UploadSolidStyle{
			bez.width,
			capcapjoin,
			{0.0f,0.0f }
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

	calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);

	ClearComputeBindings(context);
}

void BezierSolidRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();

	UpdateBuffers(device, context);

	if (total_points < 2 || !calculated_points) return;

	UploadCameraData(view_proj, context);

	// The point pass is world-space only here, so strictly it could be skipped on frames where
	// nothing moved. It is left unconditional to match the patterned renderer's behaviour; making
	// it conditional is a one-line change once curve motion is tracked separately from the camera.
	RunPointPass(context);

	// --- curve-body draw --------------------------------------------------------------
	curve_draw.Bind(context);

	calculated_points->BindOrdered(ShaderStage::Vertex, 0, context); // t0: points & packed colours
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);      // t1: curve boundary flags
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);          // t4: point -> curve index
	curve_styles->Bind(ShaderStage::Vertex, 6, context);             // t6: width / cap / join

	// t2 (distances), t3 (curve definitions) and t5 (pattern ranges) are unbound: nothing in the
	// solid pipeline reads them. Slots are kept at the patterned renderer's numbers so the two
	// vertex shaders stay easy to diff.

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_points - 1u, 0, 0);

	ClearDrawBindings(context);
}
