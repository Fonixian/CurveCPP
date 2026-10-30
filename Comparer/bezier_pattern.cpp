#include "bezier_pattern.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

BezierPatternRenderer::BezierPatternRenderer(const GraphicsDevice& device)
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

void BezierPatternRenderer::AllocatePointBuffers(const GraphicsDevice& device, uint32_t points_required) {
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT2>(points_required)));
}

void BezierPatternRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	// +1: the scan appends the grand total after the last curve.
	pattern_offsets.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required + 1u)));
}

void BezierPatternRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadPatternStyle>(curves_required)));
}

void BezierPatternRenderer::UploadStyles(GraphicsDeviceContext* context) {
	style_scratch.resize(TotalCurves());
	for (size_t curveIndex = 0; curveIndex < style_scratch.size(); ++curveIndex)
		style_scratch[curveIndex] = UploadPatternStyle{ style_words[curveIndex], spacings[curveIndex], dash_lengths[curveIndex] };
	curve_styles->Upload(std::span<const UploadPatternStyle>{ style_scratch }, context);
}

void BezierPatternRenderer::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	BezierRendererBase::UploadCurves(context, parts);

	if (parts & (DirtyPositions | DirtyStyles))
		UpdatePatternBound();
	if (parts & (DirtyPositions | DirtyStyles | DirtyLayout))
		need_recount = true;
}

void BezierPatternRenderer::RunPointPass(GraphicsDeviceContext* context) {
	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);        // b0
	curve_control_points->Bind(ShaderStage::Compute, 0, context); // t0: P0..P3 per curve
	bezier_data_map->Bind(ShaderStage::Compute, 1, context);      // t1: owning curve per sample
	curve_indices->Bind(ShaderStage::Compute, 2, context);        // t2: sample range per curve
	distances->BindUnordered(0, context);                         // u0: (world, screen) per sample

	profiler.begin_gpu("calc");
	calc_points->Run({ GroupCount(total_points, 256u), 1u, 1u }, context);
	profiler.end_gpu("calc");

	ClearComputeBindings(context);

	profiler.begin_gpu("scan");
	distance_scan.Scan(*distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");

	ClearComputeBindings(context);
}

// Sized from the CPU bound, never from a GPU result.
void BezierPatternRenderer::AllocatePatternBuffer(const GraphicsDevice& device) {
	const uint32_t required = FitCapacity(patterns_allocated, pattern_upper_bound);
	if (patterns && patterns_allocated == required) return;

	patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));
	patterns_allocated = required;
}

// Count per curve, then an exclusive scan into offsets (no atomics, so the layout is deterministic).
void BezierPatternRenderer::CountPatternCenters(GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(TotalCurves());

	if (curve_count == 0u || !pattern_offsets) return;

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);            // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);         // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);             // t2
	pattern_offsets->BindUnordered(0, context);                       // u0

	pattern_ini->Run({ GroupCount(curve_count, 256u), 1u, 1u }, context);

	ClearComputeBindings(context);

	offset_scan.Scan(*pattern_offsets, curve_count, context, true);

	ClearComputeBindings(context);
}

void BezierPatternRenderer::RunPatternPass(GraphicsDeviceContext* context) {
	if (pattern_upper_bound == 0 || !patterns) return;

	const auto curve_count = static_cast<uint32_t>(TotalCurves());

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);            // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);         // t1: (world, screen) arc length
	curve_begins->Bind(ShaderStage::Compute, 2, context);             // t2: chain begin bits
	curve_styles->Bind(ShaderStage::Compute, 3, context);             // t3
	pattern_offsets->BindOrdered(ShaderStage::Compute, 4, context);   // t4: base index per curve
	patterns->BindUnordered(0, context);                              // u0

	// 8 threads per curve on x, 8 curves per group on y.
	pattern_calc->Run({ 1u, GroupCount(curve_count, 8u), 1u }, context);

	ClearComputeBindings(context);
}

void BezierPatternRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !distances || !HasCurveData()) {
		EndDraw();
		return;
	}

	AllocatePatternBuffer(device);

	UploadCameraData(view_proj, context);

	// Screen arc length depends on the camera, so these run every frame; only the count is cached.
	RunPointPass(context);

	profiler.begin_gpu("pattern");

	if (need_recount) {
		CountPatternCenters(context);
		need_recount = false;
	}

	RunPatternPass(context);

	profiler.end_gpu("pattern");

	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	// t1, t3..t7 match the solid renderer.
	distances->BindOrdered(ShaderStage::Vertex, 0, context);           // t0: (world, screen) arc length
	curve_begins->Bind(ShaderStage::Vertex, 1, context);               // t1: chain begin bits
	BindCurveData(context, 3, 5, 7);                                   // t3 control points, t5 colours, t7 indices
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);            // t4: point -> curve index
	curve_styles->Bind(ShaderStage::Vertex, 6, context);               // t6: width_capcapjoin / spacing / dash length
	pattern_offsets->BindOrdered(ShaderStage::Vertex, 8, context);     // t8: pattern base per curve

	// t1: centres; a chain's last one carries the sign bit.
	if (patterns) patterns->BindOrdered(ShaderStage::Pixel, 1, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_points - 1u, 0, 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
