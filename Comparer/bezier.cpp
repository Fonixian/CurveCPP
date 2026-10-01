#include "bezier.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

constexpr uint32_t maxElementCount = 1'200'000u;

// Same packing as the solid renderer's style word - see UploadPatternStyle in bezier.h.
static uint32_t PackWidthCapCapJoin(const BezierData& bez) {
	const uint32_t width = static_cast<uint32_t>(std::clamp(bez.width, 0.f, 255.f) + 0.5f);
	return (width << 24) |
		(uint32_t(bez.cap_front) << 16) |
		(uint32_t(bez.cap_back) << 8) |
		uint32_t(bez.join);
}

// --- BezierRenderer ----------------------------------------------------------------------------

BezierRenderer::BezierRenderer(const GraphicsDevice& device)
	// offset_scan runs over curve counts, and a curve is worth at least one point, so maxElementCount
	// bounds the curve count too - no separate cap, and its buffers cost a few KB at that size.
	: BezierSplitRendererBase(device),
	  world_scan{ device, maxElementCount }, screen_scan{ device, maxElementCount },
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

	pattern_capacity = std::make_unique<ConstantBuffer>(device, capacity_cb_data);
}

void BezierRenderer::AllocatePointBuffers(const GraphicsDevice& device, uint32_t points_required) {
	// Two float buffers rather than one of float2: same bytes, but each pass loads only the channel
	// it actually reads, and each is scanned by its own SegmentedScan. See the note in bezier.h.
	world_distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(points_required)));
	screen_distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(points_required)));
}

void BezierRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	// Control points, colours and sample ranges.
	BezierSplitRendererBase::AllocateCurveBuffers(device, curves_required);

	// One element longer than the curve count on purpose: the scan parks the grand total in the slot
	// just past the last curve, and a structured-buffer UAV drops an out-of-range store without
	// complaining, so a buffer sized exactly to curves_required would lose the total silently rather
	// than fault. curve_pattern_calc reads [i + 1] for its count and curve_vs reads
	// [last curve of the chain + 1] as the end of the chain's slot range - the appended total when
	// that chain is the scene's last - so the slot is live geometry, not slack.
	pattern_offsets.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required + 1u)));

	curve_chains.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadChainCurves>(curves_required)));
}

void BezierRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadPatternStyle>(curves_required)));
}

void BezierRenderer::UploadStyles(GraphicsDeviceContext* context) {
	style_scratch.resize(curves.size());
	for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
		const BezierData& bez = curves[curveIndex];
		style_scratch[curveIndex] = UploadPatternStyle{ PackWidthCapCapJoin(bez), bez.spacing, bez.dash_length };
	}
	curve_styles->Upload(std::span<const UploadPatternStyle>{ style_scratch }, context);
}

// Same sum UploadCurveData forms, over the same cubic control polygons - see PatternBound(). It
// needs positions AND spacings, so it is redone when either changed.
void BezierRenderer::UpdatePatternBound() {
	uint64_t bound = 0;
	for (const BezierData& bez : curves) {
		XMFLOAT3 p0, p1, p2, p3;
		ToCubic(bez, p0, p1, p2, p3);
		bound += PatternCenterBound(p0, p1, p2, p3, bez.spacing);
	}
	pattern_upper_bound = static_cast<uint32_t>(std::min<uint64_t>(bound, maxPatternCount));
}

void BezierRenderer::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	BezierSplitRendererBase::UploadCurves(context, parts);

	// Width, caps and join are styles too, so changing only those still recounts; harmless, not free.
	if (parts & (DirtyPositions | DirtyStyles))
		UpdatePatternBound();
	if (parts & (DirtyPositions | DirtyStyles | DirtyLayout))
		need_recount = true;

	// The chain each curve belongs to, as a curve range. A chain is a run of consecutive curves that
	// starts at IsChainStart(), so the last curve of one is the curve before the next start.
	if ((parts & DirtyLayout) && curve_chains && !curves.empty()) {
		chain_scratch.resize(curves.size());
		size_t chain_first = 0;
		for (size_t curveIndex = 1; curveIndex <= curves.size(); ++curveIndex) {
			if (curveIndex == curves.size() || IsChainStart(curveIndex)) {
				for (size_t k = chain_first; k < curveIndex; ++k)
					chain_scratch[k] = UploadChainCurves{ uint32_t(chain_first), uint32_t(curveIndex - 1) };
				chain_first = curveIndex;
			}
		}
		curve_chains->Upload(std::span<const UploadChainCurves>{ chain_scratch }, context);
	}
}

void BezierRenderer::RunPointPass(GraphicsDeviceContext* context) {
	ClearComputeBindings(context);

	// No CalculatedPoints: the point pass measures the two chord lengths and drops the positions -
	// curve_vs evaluates the ones it draws itself (NeedsCalculatedPoints is false).
	viewport_data->Bind(ShaderStage::Compute, 0, context);        // b0
	curve_control_points->Bind(ShaderStage::Compute, 0, context); // t0: K0..K3 per curve
	bezier_data_map->Bind(ShaderStage::Compute, 1, context);      // t1: owning curve per sample
	curve_indices->Bind(ShaderStage::Compute, 2, context);        // t2: sample range per curve
	world_distances->BindUnordered(0, context);                   // u0
	screen_distances->BindUnordered(1, context);                  // u1

	profiler.begin_gpu("calc");
	calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);
	profiler.end_gpu("calc");

	ClearComputeBindings(context);

	// Two independent scans over the same begin-bit flags. `curve_begins` is read-only to the
	// top-level pass (only the lower levels write their own block flags, and those live inside the
	// instance), so the two share it safely; they need separate instances only for the block-sum and
	// carry scratch. Both under one "scan" metric, as the single float2 scan was.
	profiler.begin_gpu("scan");
	world_scan.Scan(*world_distances, *curve_begins, total_points, context);
	screen_scan.Scan(*screen_distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");

	ClearComputeBindings(context);
}

// Sized from the CPU-side upper bound, so this runs before any compute pass and never waits on one.
// Rounded up by next_pow2 on top of a bound that already over-counts, and shrunk only at a quarter,
// so a scene edit usually costs no reallocation at all.
void BezierRenderer::AllocatePatternBuffer(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	// Grow when the bound no longer fits, shrink once it has fallen to a quarter of the allocation
	// (removed curves, a larger spacing) - see FitCapacity() in bezier_common.
	const uint32_t required = FitCapacity(patterns_allocated, pattern_upper_bound);
	if (patterns && patterns_allocated == required) return;

	patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));
	patterns_allocated = required;

	capacity_cb_data.capacity = patterns_allocated;
	pattern_capacity->Upload(capacity_cb_data, context);
}

// Count, then scan. No atomic anywhere in here, so curve i's slice of the pattern array is a
// function of the curve data alone - reproducible frame to frame, run to run, and across GPUs.
//
// There used to be a third dispatch after the scan - curve_pattern_resolve - to fold the offsets
// back into a uint2's .x and to publish the grand total. Neither survives: the counts are read as
// the gap between neighbouring offsets, and ParalellScan appends the total itself.
void BezierRenderer::CountPatternCenters(GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(curves.size());

	if (curve_count == 0u || !pattern_offsets) return;

	ClearComputeBindings(context);

	// 1. Per curve, how many centres it has. Each thread writes only its own element now.
	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	curve_indices->Bind(ShaderStage::Compute, 0, context);            // t0: sample range per curve
	// World only - the count depends on world arc length alone, so screen_distances is not bound.
	world_distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);             // t2
	pattern_offsets->BindUnordered(0, context);                 // u0

	pattern_ini->Run({ (curve_count + 256u - 1u) / 256u, 1u, 1u }, context);

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

	const auto curve_count = static_cast<uint32_t>(curves.size());

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	pattern_capacity->Bind(ShaderStage::Compute, 1, context);         // b1
	curve_indices->Bind(ShaderStage::Compute, 0, context);            // t0: sample range per curve
	world_distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1: binary-search key
	screen_distances->BindOrdered(ShaderStage::Compute, 2, context);  // t2: what the hit interpolates
	curve_styles->Bind(ShaderStage::Compute, 3, context);             // t3
	pattern_offsets->BindOrdered(ShaderStage::Compute, 4, context);   // t4: base index per curve, total appended
	patterns->BindUnordered(0, context);                              // u0

	// 8 threads per curve on x, 8 curves per group on y.
	pattern_calc->Run({ 1u, (curve_count + 8u - 1u) / 8u, 1u }, context);

	ClearComputeBindings(context);
}

void BezierRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	// need_recount is raised inside UploadCurves, only for the parts the count depends on.
	UpdateBuffers(device, context);

	if (total_points < 2 || !world_distances || !HasCurveData()) {
		EndDraw();
		return;
	}

	// CPU-side and independent of anything the GPU is doing: it reads PatternBound() only.
	AllocatePatternBuffer(device, context);

	UploadCameraData(view_proj, context);

	// Screen-space arc length depends on the camera, so the point pass and the pattern pass rerun
	// every frame. Only the centre COUNT is camera-independent.
	RunPointPass(context);

	// ini + scan + calc under one metric. On a steady frame need_recount is false and this is calc
	// alone; the frames that recount add the count/scan pair on top. It still carries no readback
	// stall, so this row should stay flat across a scene change instead of spiking.
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

	// t1 and t3..t7 are exactly the solid renderer's bindings; the four buffers only this renderer
	// has take t0, t2, t8 and t9.
	world_distances->BindOrdered(ShaderStage::Vertex, 0, context);     // t0: cumulative world arc length
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);        // t1: chain boundary flags
	screen_distances->BindOrdered(ShaderStage::Vertex, 2, context);    // t2: cumulative screen arc length, px
	BindCurveData(context, 3, 5, 7);                                   // t3 control points, t5 colours, t7 indices
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);            // t4: point -> curve index
	curve_styles->Bind(ShaderStage::Vertex, 6, context);               // t6: width_capcapjoin / spacing / dash length
	pattern_offsets->BindOrdered(ShaderStage::Vertex, 8, context);     // t8: pattern base per curve
	curve_chains->Bind(ShaderStage::Vertex, 9, context);               // t9: first/last curve of the chain

	// t1: screen arc length per pattern center. Its clamp bound used to be read here too, from
	// pattern_offsets[TotalCurveCount] - the SCENE's centre total, which let a curve read the next
	// chain's centres. The bound is the chain's slot range now, and it arrives as an interpolant
	// (PatternSlots) from curve_vs, so pattern_offsets is no longer bound to the pixel stage.
	if (patterns) patterns->BindOrdered(ShaderStage::Pixel, 1, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_points - 1u, 0, 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
