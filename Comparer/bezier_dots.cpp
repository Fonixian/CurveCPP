#include "bezier_dots.h"

using namespace Axodox::Graphics;
using namespace DirectX;

struct DotSample {
	float    t;
	uint32_t curve_index;
};
static_assert(sizeof(DotSample) == 8, "DotSample must match DotSample in the shaders");

BezierDotRenderer::BezierDotRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device), draw_args{ device },
	  scan{ device, maxElementCount }, offset_scan{ device, maxElementCount }
{
	calc_points = Pipeline::getCS(device, "dot_calc_points.cso");
	dot_ini = Pipeline::getCS(device, "dot_ini.cso");
	dot_calc = Pipeline::getCS(device, "dot_calc.cso");
	dot_args = Pipeline::getCS(device, "dot_args.cso");

	curve_draw.vs = Pipeline::getVS(device, "dot_vert.cso");
	curve_draw.ps = Pipeline::getPS(device, "dot_ps.cso");
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});
}

void BezierDotRenderer::AllocatePointBuffers(const GraphicsDevice& device, uint32_t points_required) {
	AllocateSampleMap(device, points_required);   // point pass
	AllocateChainBegins(device, points_required); // distance scan
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(points_required)));
}

void BezierDotRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	dot_indices.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required + 1u)));
}

void BezierDotRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadDotStyle>(curves_required)));
}

void BezierDotRenderer::UploadStyles(GraphicsDeviceContext* context) {
	style_scratch.resize(TotalCurves());
	for (size_t curveIndex = 0; curveIndex < style_scratch.size(); ++curveIndex)
		style_scratch[curveIndex] = UploadDotStyle{ style_words[curveIndex] & ~0xFFu, spacings[curveIndex] };
	curve_styles->Upload(std::span<const UploadDotStyle>{ style_scratch }, context);
}

void BezierDotRenderer::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	BezierRendererBase::UploadCurves(context, parts);

	if (parts & (DirtyPositions | DirtyStyles))
		UpdatePatternBound();
	if (parts & (DirtyPositions | DirtyStyles | DirtyLayout))
		need_recount = true;
}

void BezierDotRenderer::RunPointPass(GraphicsDeviceContext* context) {
	viewport_data->Bind(ShaderStage::Compute, 0, context);        // b0
	curve_control_points->Bind(ShaderStage::Compute, 0, context); // t0: one 3x4 per curve, columns P0..P3
	bezier_data_map->Bind(ShaderStage::Compute, 1, context);      // t1: owning curve per sample
	curve_indices->Bind(ShaderStage::Compute, 2, context);        // t2: sample range per curve
	distances->BindUnordered(0, context);                         // u0

	profiler.begin_gpu("calc");
	calc_points->Run({ GroupCount(total_points, 512u), 1u, 1u }, context); // 256 threads x 2 points each
	profiler.end_gpu("calc");

	UnbindShaderResources(context, ShaderStage::Compute, { 0, 1, 2 });
	UnbindUnorderedAccessViews(context, { 0 });

	profiler.begin_gpu("scan");
	scan.Scan(*distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");
}

void BezierDotRenderer::AllocateDotBuffer(const GraphicsDevice& device) {
	const uint32_t required = FitCapacity(patterns_allocated, pattern_upper_bound);
	if (dots && patterns_allocated == required) return;

	dots.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<DotSample>(required)));
	patterns_allocated = required;
}

void BezierDotRenderer::CountDots(GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(TotalCurves());

	if (curve_count == 0u || !dot_indices) return;

	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);      // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);       // t2
	dot_indices->BindUnordered(0, context);                     // u0

	dot_ini->Run({ GroupCount(curve_count, 64u), 1u, 1u }, context);

	UnbindShaderResources(context, ShaderStage::Compute, { 0, 1, 2 });
	UnbindUnorderedAccessViews(context, { 0 });

	offset_scan.Scan(*dot_indices, curve_count, context, true);

	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	dot_indices->BindUnordered(0, context);                     // u0
	draw_args.BindUnordered(1, context);                        // u1

	dot_args->Run({ 1u, 1u, 1u }, context);

	UnbindUnorderedAccessViews(context, { 0, 1 });
}

void BezierDotRenderer::RunDotPlacementPass(GraphicsDeviceContext* context) {
	if (pattern_upper_bound == 0 || !dots) return;

	const auto curve_count = static_cast<uint32_t>(TotalCurves());

	viewport_data->Bind(ShaderStage::Compute, 0, context);          // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);          // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);       // t1
	curve_styles->Bind(ShaderStage::Compute, 3, context);           // t3
	dot_indices->BindOrdered(ShaderStage::Compute, 4, context);     // t4: base index per curve, total appended
	dots->BindUnordered(0, context);                                // u0

	dot_calc->Run({ 1u, GroupCount(curve_count, 8u), 1u }, context);

	UnbindShaderResources(context, ShaderStage::Compute, { 0, 1, 3, 4 });
	UnbindUnorderedAccessViews(context, { 0 });
}

void BezierDotRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !distances || !HasCurveData()) {
		EndDraw();
		return;
	}

	AllocateDotBuffer(device);

	UploadCameraData(view_proj, context);

	RunPointPass(context);

	profiler.begin_gpu("pattern");

	if (need_recount) {
		CountDots(context);
		need_recount = false;
	}

	RunDotPlacementPass(context);

	profiler.end_gpu("pattern");

	if (pattern_upper_bound == 0 || !dots) {
		EndDraw();
		return;
	}

	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	curve_control_points->Bind(ShaderStage::Vertex, 3, context);     // t3: control points
	dots->BindOrdered(ShaderStage::Vertex, 4, context);              // t4: per-dot curve t + curve index
	curve_colors->Bind(ShaderStage::Vertex, 5, context);             // t5: colours
	curve_styles->Bind(ShaderStage::Vertex, 6, context);             // t6: width_capcap + spacing

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	context->get()->DrawInstancedIndirect(draw_args.get(), 0);
	profiler.end_gpu("draw");

	UnbindShaderResources(context, ShaderStage::Vertex, { 3, 4, 5, 6 });
	EndDraw();
}
