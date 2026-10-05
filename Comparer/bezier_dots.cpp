#include "bezier_dots.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

constexpr uint32_t maxElementCount = 1'200'000u;

static uint32_t PackDotStyle(const BezierData& bez) {
	const uint32_t width = static_cast<uint32_t>(std::clamp(bez.width, 0.f, 255.f) + 0.5f);
	return (width << 24) |
		(uint32_t(bez.cap_front) << 16) |
		(uint32_t(bez.cap_back) << 8);
}

struct DotSample {
	uint32_t sample;
	float    segmentT; // sample, sample + 1
	uint32_t curve_index;
};

// --- BezierDotRenderer -------------------------------------------------------------------------

BezierDotRenderer::BezierDotRenderer(const GraphicsDevice& device)
	// offset_scan runs over curve counts, and a curve is worth at least one point, so maxElementCount
	// bounds the curve count too - no separate cap, and its buffers cost a few KB at that size.
	: BezierSplitRendererBase(device), draw_args{ device },
	  scan{ device, maxElementCount }, offset_scan{ device, maxElementCount }
{
	calc_points = Pipeline::getCS(device, "dot_calc_points.cso");
	dot_ini = Pipeline::getCS(device, "dot_ini.cso");
	dot_calc = Pipeline::getCS(device, "dot_calc.cso");
	dot_args = Pipeline::getCS(device, "dot_args.cso");

	curve_draw.vs = Pipeline::getVS(device, "dot_vert.cso");
	curve_draw.ps = Pipeline::getPS(device, "dot_ps.cso");
	// AlphaBlend for the SDF antialiasing at each dot's edge, same as the other two renderers.
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});

	dot_capacity = std::make_unique<ConstantBuffer>(device, capacity_cb_data);
}

void BezierDotRenderer::AllocatePointBuffers(const GraphicsDevice& device, uint32_t points_required) {
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(points_required)));
}

void BezierDotRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	// Control points, colours and sample ranges.
	BezierSplitRendererBase::AllocateCurveBuffers(device, curves_required);

	// One element longer than the curve count on purpose: the scan parks the grand total in the slot
	// just past the last curve, and a structured-buffer UAV drops an out-of-range store without
	// complaining, so a buffer sized exactly to curves_required would lose the total silently rather
	// than fault. dot_calc reads [i + 1], so that slot is live geometry, not slack.
	dot_indices.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required + 1u)));
}

void BezierDotRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadDotStyle>(curves_required)));
}

void BezierDotRenderer::UploadStyles(GraphicsDeviceContext* context) {
	style_scratch.resize(curves.size());
	for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex)
		style_scratch[curveIndex] = UploadDotStyle{ PackDotStyle(curves[curveIndex]), curves[curveIndex].spacing };
	curve_styles->Upload(std::span<const UploadDotStyle>{ style_scratch }, context);
}

// Same sum UploadCurveData forms for the patterned renderer, over the same cubic control polygons -
// see PatternBound(). It needs positions AND spacings, so it is redone when either changed.
void BezierDotRenderer::UpdatePatternBound() {
	uint64_t bound = 0;
	for (const BezierData& bez : curves) {
		XMFLOAT3 p0, p1, p2, p3;
		ToCubic(bez, p0, p1, p2, p3);
		bound += PatternCenterBound(p0, p1, p2, p3, bez.spacing);
	}
	pattern_upper_bound = static_cast<uint32_t>(std::min<uint64_t>(bound, maxPatternCount));
}

void BezierDotRenderer::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	BezierSplitRendererBase::UploadCurves(context, parts);

	// Width and caps are styles too, so a width-only change still recounts; harmless, just not free.
	if (parts & (DirtyPositions | DirtyStyles))
		UpdatePatternBound();
	if (parts & (DirtyPositions | DirtyStyles | DirtyLayout))
		need_recount = true;
}

void BezierDotRenderer::RunPointPass(GraphicsDeviceContext* context) {
	ClearComputeBindings(context);

	// No CalculatedPoints here: the point pass measures chord lengths and drops the positions, and
	// this renderer never allocates the buffer (NeedsCalculatedPoints).
	viewport_data->Bind(ShaderStage::Compute, 0, context);        // b0
	curve_control_points->Bind(ShaderStage::Compute, 0, context); // t0: P0..P3 per curve
	bezier_data_map->Bind(ShaderStage::Compute, 1, context);      // t1: owning curve per sample
	curve_indices->Bind(ShaderStage::Compute, 2, context);        // t2: sample range per curve
	distances->BindUnordered(0, context);                         // u0

	profiler.begin_gpu("calc");
	calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);
	profiler.end_gpu("calc");

	ClearComputeBindings(context);

	// World arc length only, one float per point. The scan is scalar now, so there is no longer a
	// zero-filled second channel riding along with it: see bezier_dots.h.
	profiler.begin_gpu("scan");
	scan.Scan(*distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");

	ClearComputeBindings(context);
}

// Sized from the CPU-side upper bound, so this runs before any compute pass and never waits on one.
// Grow / shrink-at-a-quarter; see BezierRenderer::AllocatePatternBuffer for the same reasoning.
void BezierDotRenderer::AllocateDotBuffer(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	// Grow when the bound no longer fits, shrink once it has fallen to a quarter of the allocation
	// (removed curves, a larger spacing) - see FitCapacity() in bezier_common.
	const uint32_t required = FitCapacity(dots_allocated, pattern_upper_bound);
	if (dots && dots_allocated == required) return;

	dots.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<DotSample>(required)));
	dots_allocated = required;

	capacity_cb_data.capacity = dots_allocated;
	dot_capacity->Upload(capacity_cb_data, context);
}

// Count, scan, then the indirect args. No atomic anywhere in here, so curve i's slice of the dot
// array is a function of the curve data alone - reproducible frame to frame, run to run, and across
// GPUs. It matters more here than in the patterned renderer: a dot's index IS its instance id, so a
// reshuffled layout used to mean a reshuffled draw.
//
// There used to be a third dispatch between the scan and the args - dot_resolve - whose whole job was
// to fold the scanned offsets back into a uint2's .x and add the last offset to the last count to
// publish a total. Splitting the ranges into two uint buffers removed the first half of that, and
// teaching ParalellScan to append its total removed the second, so the pass is gone.
void BezierDotRenderer::CountDots(GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(curves.size());

	if (curve_count == 0u || !dot_indices) return;

	ClearComputeBindings(context);

	// 1. Per curve, how many dots it has, written to both buffers. Each thread writes only its own
	// element - no atomic, no contention.
	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);      // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);       // t2
	dot_indices->BindUnordered(0, context);                     // u0

	dot_ini->Run({ (curve_count + 64u - 1u) / 64u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// 2. Exclusive prefix sum over those counts, in place: dot_indices[i] becomes the number of dots
	// in curves 0..i-1, which is exactly curve i's base index into the flat dot array. appendTotal
	// also leaves the grand total in dot_indices[curve_count] - the buffer is allocated one long for
	// it - which is what makes every count recoverable as indices[i + 1] - indices[i].
	offset_scan.Scan(*dot_indices, curve_count, context, true);

	ClearComputeBindings(context);

	// 3. One thread, lifting that appended total into the indirect draw arguments. This is the step
	// that replaces the readback: the instance count reaches the draw without ever touching the CPU,
	// so it is always the CURRENT frame's count. The scan binds its own constants at b0, hence the
	// rebind of viewport_data here.
	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	dot_capacity->Bind(ShaderStage::Compute, 1, context);       // b1
	dot_indices->BindUnordered(0, context);                     // u0
	draw_args.BindUnordered(1, context);                        // u1

	dot_args->Run({ 1u, 1u, 1u }, context);

	ClearComputeBindings(context);
}

void BezierDotRenderer::RunDotPlacementPass(GraphicsDeviceContext* context) {
	// Gated on the bound rather than on a count, because the count is deliberately a few frames old.
	// A bound of 0 means no curve in the scene has a positive spacing, which is current and exact.
	if (pattern_upper_bound == 0 || !dots) return;

	const auto curve_count = static_cast<uint32_t>(curves.size());

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);          // b0
	dot_capacity->Bind(ShaderStage::Compute, 1, context);           // b1
	curve_indices->Bind(ShaderStage::Compute, 0, context);          // t0: sample range per curve
	distances->BindOrdered(ShaderStage::Compute, 1, context);       // t1
	curve_styles->Bind(ShaderStage::Compute, 3, context);           // t3
	dot_indices->BindOrdered(ShaderStage::Compute, 4, context);     // t4: base index per curve, total appended
	dots->BindUnordered(0, context);                                // u0

	// Same 8 threads per curve on x, 8 curves per group on y shape as the patterned renderer's
	// pattern_calc - the binary search per dot is the same cost either way.
	dot_calc->Run({ 1u, (curve_count + 8u - 1u) / 8u, 1u }, context);

	ClearComputeBindings(context);
}

void BezierDotRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	// need_recount is raised inside UploadCurves, only for the parts the count depends on.
	UpdateBuffers(device, context);

	if (total_points < 2 || !distances || !HasCurveData()) {
		EndDraw();
		return;
	}

	// CPU-side and independent of anything the GPU is doing: it reads PatternBound() only.
	AllocateDotBuffer(device, context);

	UploadCameraData(view_proj, context);

	// Screen-space projection happens per-vertex in dot_vert.hlsl, straight from CalculatedPoints, so
	// unlike the patterned renderer nothing here depends on the camera except that final projection -
	// the point pass and the dot count are both pure world-space work. Still rerun every frame to
	// match the other two renderers' behaviour (see BezierSolidRenderer::Draw for the same note).
	RunPointPass(context);

	// Named "pattern" rather than "dots" so it lines up with the patterned renderer's row. The stages
	// no longer match it exactly: ini + scan + args + calc here against the patterned renderer's
	// ini + scan + resolve + calc. No readback stall on the recount frames either way, so this row
	// should stay flat across a scene change.
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

	// --- dot draw ---------------------------------------------------------------------
	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	// Nothing per sample point is bound. dot_vert evaluates the two samples a dot sits between out of
	// the control points and takes its curve index from the dot itself. The per-curve buffers sit in
	// solid_vert's slots; the dots take t4, which is the index map there.
	BindCurveData(context, 3, 5, 7);                                 // t3 control points, t5 colours, t7 indices
	dots->BindOrdered(ShaderStage::Vertex, 4, context);              // t4: per-dot bracketing samples + t + curve
	curve_styles->Bind(ShaderStage::Vertex, 6, context);             // t6: width_capcap + spacing

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// The instance count lives in draw_args, written by dot_args on the GPU. A dot-free scene leaves
	// it at 0 and this draws nothing, which is why there is no count to test against here.
	context->get()->DrawInstancedIndirect(draw_args.get(), 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
