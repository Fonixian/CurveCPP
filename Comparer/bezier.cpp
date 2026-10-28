#include "bezier.h"

using namespace Axodox::Graphics;
using namespace DirectX;

BezierRenderer::BezierRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device),
	  distance_scan{ device, maxElementCount },
	  offset_scan{ device, maxElementCount }
{
	calc_points = Pipeline::getCS(device, "curve_calc_points.cso");
	pattern_ini = Pipeline::getCS(device, "curve_pattern_ini.cso");
	pattern_calc = Pipeline::getCS(device, "curve_pattern_calc.cso");

	curve_draw.vs = Pipeline::getVS(device, "curve_vs.cso");
	curve_draw.ps = Pipeline::getPS(device, "curve_ps.cso");
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});
}

void BezierRenderer::AllocatePointBuffers(const GraphicsDevice& device, uint32_t points_required) {
	AllocateSampleMap(device, points_required);   // point pass
	AllocateChainBegins(device, points_required); // distance scan, curve_pattern_calc
	AllocateSegmentMap(device, points_required);  // curve-body draw
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT2>(points_required)));
}

void BezierRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	pattern_offsets.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required + 1u)));
}

void BezierRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadPatternStyle>(curves_required)));
}

void BezierRenderer::UploadStyles(GraphicsDeviceContext* context) {
	style_scratch.resize(TotalCurves());
	for (size_t curveIndex = 0; curveIndex < style_scratch.size(); ++curveIndex)
		style_scratch[curveIndex] = UploadPatternStyle{ style_words[curveIndex], spacings[curveIndex], dash_lengths[curveIndex] };
	curve_styles->Upload(std::span<const UploadPatternStyle>{ style_scratch }, context);
}

void BezierRenderer::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	BezierRendererBase::UploadCurves(context, parts);

	// Width, caps and join are styles too, so changing only those still recounts; harmless, not free.
	if (parts & (DirtyPositions | DirtyStyles))
		UpdatePatternBound();
	if (parts & (DirtyPositions | DirtyStyles | DirtyLayout))
		need_recount = true;
}

void BezierRenderer::RunPointPass(GraphicsDeviceContext* context) {
	viewport_data->Bind(ShaderStage::Compute, 0, context);        // b0
	curve_control_points->Bind(ShaderStage::Compute, 0, context); // t0: one 3x4 per curve, columns P0..P3
	bezier_data_map->Bind(ShaderStage::Compute, 1, context);      // t1: owning curve per sample
	curve_indices->Bind(ShaderStage::Compute, 2, context);        // t2: sample range per curve
	distances->BindUnordered(0, context);                         // u0: (world, screen) per sample

	profiler.begin_gpu("calc");
	calc_points->Run({ GroupCount(total_points, 256u), 1u, 1u }, context);
	profiler.end_gpu("calc");

	UnbindShaderResources(context, ShaderStage::Compute, { 0, 1, 2 });
	UnbindUnorderedAccessViews(context, { 0 });

	profiler.begin_gpu("scan");
	distance_scan.Scan(*distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");
}

void BezierRenderer::AllocatePatternBuffer(const GraphicsDevice& device) {
	const uint32_t required = FitCapacity(patterns_allocated, pattern_upper_bound);
	if (patterns && patterns_allocated == required) return;

	patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));
	patterns_allocated = required;
}

void BezierRenderer::CountPatternCenters(GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(TotalCurves());

	if (curve_count == 0u || !pattern_offsets) return;

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);            // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);         // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);             // t2
	pattern_offsets->BindUnordered(0, context);                       // u0

	pattern_ini->Run({ GroupCount(curve_count, 256u), 1u, 1u }, context);

	UnbindShaderResources(context, ShaderStage::Compute, { 0, 1, 2 });
	UnbindUnorderedAccessViews(context, { 0 });

	offset_scan.Scan(*pattern_offsets, curve_count, context, true);
}

void BezierRenderer::RunPatternPass(GraphicsDeviceContext* context) {
	if (pattern_upper_bound == 0 || !patterns) return;

	const auto curve_count = static_cast<uint32_t>(TotalCurves());

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);            // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);         // t1: world (x) is the binary-search key, screen (y) is what the hit interpolates
	curve_begins->Bind(ShaderStage::Compute, 2, context);      // t2: chain begin bits, to find the chain's last centre
	curve_styles->Bind(ShaderStage::Compute, 3, context);             // t3
	pattern_offsets->BindOrdered(ShaderStage::Compute, 4, context);   // t4: base index per curve, total appended
	patterns->BindUnordered(0, context);                              // u0

	// 8 threads per curve on x, 8 curves per group on y.
	pattern_calc->Run({ 1u, GroupCount(curve_count, 8u), 1u }, context);

	UnbindShaderResources(context, ShaderStage::Compute, { 0, 1, 2, 3, 4 });
	UnbindUnorderedAccessViews(context, { 0 });
}

void BezierRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !distances || !HasCurveData()) {
		EndDraw();
		return;
	}

	AllocatePatternBuffer(device);

	UploadCameraData(view_proj, context);

	RunPointPass(context);

	profiler.begin_gpu("pattern");

	if (need_recount) {
		CountPatternCenters(context);
		need_recount = false;
	}

	RunPatternPass(context);

	profiler.end_gpu("pattern");

	// --- curve-body draw --------------------------------------------------------------
	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	distances->BindOrdered(ShaderStage::Vertex, 0, context);           // t0: cumulative arc length per sample, float2 (world, screen px)
	BindCurveData(context, 3, 5, 7);                                   // t3 control points, t5 colours, t7 indices
	segment_map->Bind(ShaderStage::Vertex, 4, context);                // t4: curve + continuation per segment
	curve_styles->Bind(ShaderStage::Vertex, 6, context);               // t6: width_capcapjoin / spacing / dash length
	pattern_offsets->BindOrdered(ShaderStage::Vertex, 8, context);     // t8: pattern base per curve

	patterns->BindOrdered(ShaderStage::Pixel, 1, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_segments, 0, 0);
	profiler.end_gpu("draw");

	UnbindShaderResources(context, ShaderStage::Vertex, { 0, 3, 4, 5, 6, 7, 8 });
	UnbindShaderResources(context, ShaderStage::Pixel, { 1 });
	EndDraw();
}
