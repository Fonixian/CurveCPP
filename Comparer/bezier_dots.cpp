#include "bezier_dots.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

constexpr uint32_t maxElementCount = 1'200'000u;

// One screen-aligned quad per dot, as a triangle strip. dot_args writes this into the indirect
// argument buffer's VertexCountPerInstance field, so it has to agree with dot_vert.hlsl's corner
// numbering and with the topology set in Draw().
constexpr uint32_t dotVertexCount = 4u;

// Width / cap / spacing, as dot_common.hlsli's DotStyle reads it. Join is meaningless for dots (they
// never join), so the low byte of capcapjoin is always 0 - kept only so the packing matches the other
// two renderers' style structs bit for bit, which makes them easy to diff against each other.
struct UploadDotStyle {
	float    width;
	uint32_t capcapjoin;
	float    spacing;
	float    padding;
};

// One dot: which two consecutive curve samples (as uploaded to CalculatedPoints) bracket it, and how
// far between them (0 = at sampleA, 1 = at sampleB). dot_vert.hlsl projects both samples itself and
// interpolates in screen space, rather than carrying a pre-lerped world position, so its tangent
// direction comes from the same two projected points the position does.
struct DotSample {
	uint32_t sampleA;
	uint32_t sampleB;
	float    segmentT;
	float    padding;
};

// --- BezierDotRenderer -------------------------------------------------------------------------

BezierDotRenderer::BezierDotRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device), dot_counter{ device }, draw_args{ device, dotVertexCount }, scan{ device, maxElementCount }
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
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT2>(points_required)));
}

void BezierDotRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	dot_ranges.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMUINT2>(curves_required)));
}

void BezierDotRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadDotStyle>(curves_required)));
}

void BezierDotRenderer::UploadStyles(GraphicsDeviceContext* context) {
	std::vector<UploadDotStyle> style_data;
	style_data.reserve(curves.size());

	for (const auto& bez : curves) {
		// Packed the same way as the other two renderers' capcapjoin (front<<16 | back<<8), join bits
		// left at 0 - see the struct comment above.
		uint32_t capcapjoin =
			(uint32_t(bez.cap_front) << 16) |
			(uint32_t(bez.cap_back) << 8);

		style_data.push_back(UploadDotStyle{
			bez.width,
			capcapjoin,
			bez.spacing,
			0.0f
		});
	}

	curve_styles->Upload(std::span<const UploadDotStyle>{ style_data }, context);
}

void BezierDotRenderer::RunPointPass(GraphicsDeviceContext* context) {
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

	// World arc length only - the second channel of `distances` is always 0 and the scan sums it right
	// along with the first, harmlessly. Shared, unmodified SegmentedScan: see bezier_dots.h.
	profiler.begin_gpu("scan");
	scan.Scan(*distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");

	ClearComputeBindings(context);
}

// Sized from the CPU-side upper bound, so this runs before any compute pass and never waits on one.
// Grow-only; see BezierRenderer::AllocatePatternBuffer for the same reasoning.
void BezierDotRenderer::AllocateDotBuffer(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	const uint32_t required = next_pow2(std::max(pattern_upper_bound, 1u));
	if (dots && dots_allocated >= required) return;

	dots.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<DotSample>(required)));
	dots_allocated = required;

	capacity_cb_data.capacity = dots_allocated;
	dot_capacity->Upload(capacity_cb_data, context);
}

void BezierDotRenderer::CountDots(GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(curves.size());

	// One-way CPU->GPU write, not a synchronisation point.
	dot_counter.Reset(context);

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	bezier_data->Bind(ShaderStage::Compute, 0, context);        // t0
	distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);       // t2
	dot_counter.BindUnordered(0, context);                      // u0
	dot_ranges->BindUnordered(1, context);                      // u1

	dot_ini->Run({ (curve_count + 64u - 1u) / 64u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// One thread, reading the total dot_ini just accumulated and writing it into the indirect draw
	// arguments. This is the step that replaces the readback: the instance count reaches the draw
	// without ever touching the CPU, so it is always the CURRENT frame's count.
	dot_capacity->Bind(ShaderStage::Compute, 1, context);       // b1
	dot_counter.BindOrdered(ShaderStage::Compute, 0, context);  // t0
	draw_args.BindUnordered(0, context);                        // u0

	dot_args->Run({ 1u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// Queues the copy and returns; the value turns up in a later frame's Fetch(). Only the Timings
	// window reads it - nothing in a frame depends on it any more.
	dot_counter.Submit(context);
}

void BezierDotRenderer::RunDotPlacementPass(GraphicsDeviceContext* context) {
	// Gated on the bound rather than on a count, because the count is deliberately a few frames old.
	// A bound of 0 means no curve in the scene has a positive spacing, which is current and exact.
	if (pattern_upper_bound == 0 || !dots) return;

	const auto curve_count = static_cast<uint32_t>(curves.size());

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);          // b0
	dot_capacity->Bind(ShaderStage::Compute, 1, context);           // b1
	bezier_data->Bind(ShaderStage::Compute, 0, context);            // t0
	distances->BindOrdered(ShaderStage::Compute, 1, context);       // t1
	curve_styles->Bind(ShaderStage::Compute, 3, context);           // t3
	dot_ranges->BindOrdered(ShaderStage::Compute, 4, context);      // t4
	dots->BindUnordered(0, context);                                // u0

	// Same 8 threads per curve on x, 8 curves per group on y shape as the patterned renderer's
	// pattern_calc - the binary search per dot is the same cost either way.
	dot_calc->Run({ 1u, (curve_count + 8u - 1u) / 8u, 1u }, context);

	ClearComputeBindings(context);
}

void BezierDotRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	if (UpdateBuffers(device, context)) need_recount = true;

	if (total_points < 2 || !calculated_points) {
		EndDraw();
		return;
	}

	// Both of these are CPU-side and independent of anything the GPU is doing: the allocation reads
	// PatternBound(), and the fetch takes only copies the GPU has already finished.
	AllocateDotBuffer(device, context);
	dot_counter.Fetch(context);

	UploadCameraData(view_proj, context);

	// Screen-space projection happens per-vertex in dot_vert.hlsl, straight from CalculatedPoints, so
	// unlike the patterned renderer nothing here depends on the camera except that final projection -
	// the point pass and the dot count are both pure world-space work. Still rerun every frame to
	// match the other two renderers' behaviour (see BezierSolidRenderer::Draw for the same note).
	RunPointPass(context);

	// Named "pattern" rather than "dots" so it lines up with the patterned renderer's row: same
	// stages, ini + args + calc. No readback stall on the recount frames any more, so this row should
	// stay flat across a scene change.
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

	calculated_points->BindOrdered(ShaderStage::Vertex, 0, context); // t0: points & packed colours
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);          // t4: point -> curve index
	curve_styles->Bind(ShaderStage::Vertex, 6, context);             // t6: width / caps / spacing
	dots->BindOrdered(ShaderStage::Vertex, 7, context);              // t7: per-dot bracketing samples + t

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
