#include "bezier.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

constexpr uint32_t maxElementCount = 1'200'000u;

// Width / cap / join / spacing / dash length, as curve_common.hlsli's CurveStyle reads it.
struct UploadCurveStyle {
	float    width;
	uint32_t capcapjoin;
	float    spacing;
	float    dash_length;
};

// --- BezierRenderer ----------------------------------------------------------------------------

BezierRenderer::BezierRenderer(const GraphicsDevice& device)
	// offset_scan runs over curve counts, and a curve is worth at least one point, so maxElementCount
	// bounds the curve count too - no separate cap, and its buffers cost a few KB at that size.
	: BezierRendererBase(device), pattern_counter{ device }, scan{ device, maxElementCount },
	  offset_scan{ device, maxElementCount }
{
	calc_points = Pipeline::getCS(device, "curve_calc_points.cso");
	pattern_ini = Pipeline::getCS(device, "curve_pattern_ini.cso");
	pattern_resolve = Pipeline::getCS(device, "curve_pattern_resolve.cso");
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
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT2>(points_required)));
}

void BezierRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	pattern_ranges.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMUINT2>(curves_required)));
	// The extra uint per curve the prefix sum needs; see the note in bezier.h.
	pattern_offsets.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required)));
}

void BezierRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadCurveStyle>(curves_required)));
}

void BezierRenderer::UploadStyles(GraphicsDeviceContext* context) {
	std::vector<UploadCurveStyle> style_data;
	style_data.reserve(curves.size());

	for (const auto& bez : curves) {
		uint32_t capcapjoin =
			(uint32_t(bez.cap_front) << 16) |
			(uint32_t(bez.cap_back) << 8) |
			uint32_t(bez.join);

		style_data.push_back(UploadCurveStyle{
			bez.width,
			capcapjoin,
			bez.spacing,
			bez.dash_length
		});
	}

	curve_styles->Upload(std::span<const UploadCurveStyle>{ style_data }, context);
}

void BezierRenderer::RunPointPass(GraphicsDeviceContext* context) {
	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);   // b0
	bezier_data->Bind(ShaderStage::Compute, 0, context);     // t0
	bezier_data_map->Bind(ShaderStage::Compute, 1, context); // t1
	calculated_points->BindUnordered(0, context);            // u0
	distances->BindUnordered(1, context);                    // u1

	profiler.begin_gpu("calc");
	calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);
	profiler.end_gpu("calc");

	ClearComputeBindings(context);

	profiler.begin_gpu("scan");
	scan.Scan(*distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");

	ClearComputeBindings(context);
}

// Sized from the CPU-side upper bound, so this runs before any compute pass and never waits on one.
// Grow-only, and rounded up by next_pow2 on top of a bound that already over-counts, so a scene edit
// usually costs no reallocation at all.
void BezierRenderer::AllocatePatternBuffer(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	const uint32_t required = next_pow2(std::max(pattern_upper_bound, 1u));
	if (patterns && patterns_allocated >= required) return;

	patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));
	patterns_allocated = required;

	capacity_cb_data.capacity = patterns_allocated;
	pattern_capacity->Upload(capacity_cb_data, context);
}

// Count, scan, resolve. No atomic anywhere in here, so curve i's slice of the pattern array is a
// function of the curve data alone - reproducible frame to frame, run to run, and across GPUs.
void BezierRenderer::CountPatternCenters(GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(curves.size());

	// One-way CPU->GPU write, not a synchronisation point. Still needed: with no curves at all
	// nothing below writes the counter, and a stale total would outlive the scene it was counted from.
	pattern_counter.Reset(context);

	if (curve_count == 0u || !pattern_offsets) return;

	ClearComputeBindings(context);

	// 1. Per curve, how many centres it has. Each thread writes only its own element now.
	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	bezier_data->Bind(ShaderStage::Compute, 0, context);        // t0
	distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);       // t2
	pattern_offsets->BindUnordered(0, context);                 // u0
	pattern_ranges->BindUnordered(1, context);                  // u1

	pattern_ini->Run({ (curve_count + 64u - 1u) / 64u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// 2. Exclusive prefix sum over those counts, in place: pattern_offsets[i] becomes the number of
	// centres in curves 0..i-1, which is exactly curve i's base index into the flat pattern array.
	offset_scan.Scan(*pattern_offsets, curve_count, context);

	ClearComputeBindings(context);

	// 3. Fold the offsets into pattern_ranges.x and publish the grand total. The scan binds its own
	// constants at b0, hence the rebind of viewport_data here.
	viewport_data->Bind(ShaderStage::Compute, 0, context);           // b0
	pattern_offsets->BindOrdered(ShaderStage::Compute, 0, context);  // t0
	pattern_counter.BindUnordered(0, context);                       // u0
	pattern_ranges->BindUnordered(1, context);                       // u1

	pattern_resolve->Run({ (curve_count + 64u - 1u) / 64u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// Queues the copy and returns; the value turns up in a later frame's Fetch(). Nothing this frame
	// depends on it - the pattern buffer was already sized from PatternBound().
	pattern_counter.Submit(context);
}

void BezierRenderer::RunPatternPass(GraphicsDeviceContext* context) {
	// Gated on the bound rather than on a count, because the count is deliberately a few frames old.
	// A bound of 0 means no curve in the scene has a positive spacing, which is current and exact.
	if (pattern_upper_bound == 0 || !patterns) return;

	const auto curve_count = static_cast<uint32_t>(curves.size());

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	pattern_capacity->Bind(ShaderStage::Compute, 1, context);         // b1
	bezier_data->Bind(ShaderStage::Compute, 0, context);              // t0
	distances->BindOrdered(ShaderStage::Compute, 1, context);         // t1
	curve_styles->Bind(ShaderStage::Compute, 3, context);             // t3
	pattern_ranges->BindOrdered(ShaderStage::Compute, 4, context);    // t4
	patterns->BindUnordered(0, context);                              // u0

	// 8 threads per curve on x, 8 curves per group on y.
	pattern_calc->Run({ 1u, (curve_count + 8u - 1u) / 8u, 1u }, context);

	ClearComputeBindings(context);
}

void BezierRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	if (UpdateBuffers(device, context)) need_recount = true;

	if (total_points < 2 || !calculated_points) {
		EndDraw();
		return;
	}

	// Both of these are CPU-side and independent of anything the GPU is doing: the allocation reads
	// PatternBound(), and the fetch takes only copies the GPU has already finished.
	AllocatePatternBuffer(device, context);
	pattern_counter.Fetch(context);

	UploadCameraData(view_proj, context);

	// Screen-space arc length depends on the camera, so the point pass and the pattern pass rerun
	// every frame. Only the centre COUNT is camera-independent.
	RunPointPass(context);

	// ini + scan + resolve + calc under one metric. On a steady frame need_recount is false and this
	// is calc alone; the frames that recount add the count/scan/resolve trio on top. It still carries
	// no readback stall, so this row should stay flat across a scene change instead of spiking.
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

	calculated_points->BindOrdered(ShaderStage::Vertex, 0, context); // t0: points & packed colours
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);      // t1: curve boundary flags
	distances->BindOrdered(ShaderStage::Vertex, 2, context);         // t2: cumulative (world, screen) arc length
	bezier_data->Bind(ShaderStage::Vertex, 3, context);              // t3: curve definitions
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);          // t4: point -> curve index
	pattern_ranges->BindOrdered(ShaderStage::Vertex, 5, context);    // t5: pattern range per curve
	curve_styles->Bind(ShaderStage::Vertex, 6, context);             // t6: width / cap / join / spacing / dash length

	if (patterns) patterns->BindOrdered(ShaderStage::Pixel, 1, context); // t1: screen arc length per pattern center

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_points - 1u, 0, 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
