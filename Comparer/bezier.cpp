#include "bezier.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

// --- BezierRenderer ----------------------------------------------------------------------------

BezierRenderer::BezierRenderer(const GraphicsDevice& device)
	// offset_scan runs over curve counts, and a curve is worth at least one point, so maxElementCount
	// bounds the curve count too - no separate cap, and its buffers cost a few KB at that size.
	: BezierRendererBase(device),
	  distance_scan{ device, maxElementCount },
	  offset_scan{ device, maxElementCount }
{
	calc_points = Pipeline::getCS(device, "curve_calc_points.cso");
	pattern_ini = Pipeline::getCS(device, "curve_pattern_ini.cso");
	pattern_calc = Pipeline::getCS(device, "curve_pattern_calc.cso");

	curve_draw.vs = Pipeline::getVS(device, "curve_vs.cso");
	curve_draw.ps = Pipeline::getPS(device, "curve_ps.cso");
	// AlphaBlend for the SDF antialiasing and the pattern gaps.
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});
}

void BezierRenderer::AllocatePointBuffers(const GraphicsDevice& device, uint32_t points_required) {
	// x: world arc length, y: screen arc length. One buffer, one scan. See the note in bezier.h.
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT2>(points_required)));
}

void BezierRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	// One element longer than the curve count on purpose: the scan parks the grand total in the slot
	// just past the last curve, and a structured-buffer UAV drops an out-of-range store without
	// complaining, so a buffer sized exactly to curves_required would lose the total silently rather
	// than fault. curve_pattern_calc reads [i + 1] for its count (and while looking for the end of a
	// chain, [next + 1] for every later curve), so the slot is live geometry, not slack.
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
	ClearComputeBindings(context);

	// The point pass measures the two chord lengths and drops the positions - curve_vs evaluates the
	// ones it draws itself.
	viewport_data->Bind(ShaderStage::Compute, 0, context);        // b0
	curve_control_points->Bind(ShaderStage::Compute, 0, context); // t0: P0..P3 per curve
	bezier_data_map->Bind(ShaderStage::Compute, 1, context);      // t1: owning curve per sample
	curve_indices->Bind(ShaderStage::Compute, 2, context);        // t2: sample range per curve
	distances->BindUnordered(0, context);                         // u0: (world, screen) per sample

	profiler.begin_gpu("calc");
	calc_points->Run({ GroupCount(total_points, 256u), 1u, 1u }, context);
	profiler.end_gpu("calc");

	ClearComputeBindings(context);

	// One scan carries both channels; the begin flags are the same for world and screen arc length.
	profiler.begin_gpu("scan");
	distance_scan.Scan(*distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");

	ClearComputeBindings(context);
}

// Sized from the CPU-side upper bound, so this runs before any compute pass and never waits on one.
// Grows when the bound no longer fits and shrinks once it has fallen to a quarter of the allocation
// (removed curves, a larger spacing) - see FitCapacity() - so a scene edit usually costs no
// reallocation at all.
void BezierRenderer::AllocatePatternBuffer(const GraphicsDevice& device) {
	const uint32_t required = FitCapacity(patterns_allocated, pattern_upper_bound);
	if (patterns && patterns_allocated == required) return;

	patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));
	patterns_allocated = required;
}

// Count, then scan. No atomic anywhere in here, so curve i's slice of the pattern array is a
// function of the curve data alone - reproducible frame to frame, run to run, and across GPUs. The
// counts are read as the gap between neighbouring offsets, and ParalellScan appends the total itself.
void BezierRenderer::CountPatternCenters(GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(TotalCurves());

	if (curve_count == 0u || !pattern_offsets) return;

	ClearComputeBindings(context);

	// 1. Per curve, how many centres it has. Each thread writes only its own element.
	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);            // t0: sample range per curve
	// The count depends on the world channel (x) alone; the screen channel rides along in the float2.
	distances->BindOrdered(ShaderStage::Compute, 1, context);         // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);             // t2
	pattern_offsets->BindUnordered(0, context);                       // u0

	pattern_ini->Run({ GroupCount(curve_count, 256u), 1u, 1u }, context);

	ClearComputeBindings(context);

	// 2. Exclusive prefix sum over those counts, in place: pattern_offsets[i] becomes the number of
	// centres in curves 0..i-1, which is exactly curve i's base index into the flat pattern array.
	// appendTotal also leaves the grand total in pattern_offsets[curve_count], which is what makes
	// every count recoverable as offsets[i + 1] - offsets[i] and gives curve_vs the chain total in
	// one load.
	offset_scan.Scan(*pattern_offsets, curve_count, context, true);

	ClearComputeBindings(context);
}

void BezierRenderer::RunPatternPass(GraphicsDeviceContext* context) {
	// Gated on the bound rather than on a count, because the count is deliberately a few frames old.
	// A bound of 0 means no curve in the scene has a positive spacing, which is current and exact.
	if (pattern_upper_bound == 0 || !patterns) return;

	const auto curve_count = static_cast<uint32_t>(TotalCurves());

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);            // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);         // t1: world (x) is the binary-search key, screen (y) is what the hit interpolates
	curve_begins->Bind(ShaderStage::Compute, 2, context);      // t2: chain begin bits, to find the chain's last centre
	curve_styles->Bind(ShaderStage::Compute, 3, context);             // t3
	pattern_offsets->BindOrdered(ShaderStage::Compute, 4, context);   // t4: base index per curve, total appended
	patterns->BindUnordered(0, context);                              // u0

	// 8 threads per curve on x, 8 curves per group on y.
	pattern_calc->Run({ 1u, GroupCount(curve_count, 8u), 1u }, context);

	ClearComputeBindings(context);
}

void BezierRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	// need_recount is raised inside UploadCurves, only for the parts the count depends on.
	UpdateBuffers(device, context);

	if (total_points < 2 || !distances || !HasCurveData()) {
		EndDraw();
		return;
	}

	// CPU-side and independent of anything the GPU is doing: it reads PatternBound() only.
	AllocatePatternBuffer(device);

	UploadCameraData(view_proj, context);

	// Screen-space arc length depends on the camera, so the point pass and the pattern pass rerun
	// every frame. Only the centre COUNT is camera-independent.
	RunPointPass(context);

	// ini + scan + calc under one metric. On a steady frame need_recount is false and this is calc
	// alone; the frames that recount add the count/scan pair on top.
	profiler.begin_gpu("pattern");

	if (need_recount) {
		// Runs after the point pass so the world prefix sum it reads is already valid.
		CountPatternCenters(context);
		need_recount = false;
	}

	RunPatternPass(context);

	profiler.end_gpu("pattern");

	// --- curve-body draw --------------------------------------------------------------
	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	// t1 and t3..t7 are exactly the solid renderer's bindings; the three buffers only this renderer
	// has take t0 and t8. t2 stays free, as in the solid renderer.
	distances->BindOrdered(ShaderStage::Vertex, 0, context);           // t0: cumulative arc length per sample, float2 (world, screen px)
	curve_begins->Bind(ShaderStage::Vertex, 1, context);        // t1: chain boundary flags
	BindCurveData(context, 3, 5, 7);                                   // t3 control points, t5 colours, t7 indices
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);            // t4: point -> curve index
	curve_styles->Bind(ShaderStage::Vertex, 6, context);               // t6: width_capcapjoin / spacing / dash length
	pattern_offsets->BindOrdered(ShaderStage::Vertex, 8, context);     // t8: pattern base per curve

	// t1: screen arc length per pattern center, the chain's last one flagged by its sign bit (that is
	// how the pixel shader finds the chain's end), so pattern_offsets is not bound to the pixel stage.
	if (patterns) patterns->BindOrdered(ShaderStage::Pixel, 1, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_points - 1u, 0, 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
