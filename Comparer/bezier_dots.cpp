#include "bezier_dots.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

constexpr uint32_t maxElementCount = 1'200'000u;

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
	: BezierRendererBase(device), scan{ device, maxElementCount }
{
	calc_points = Pipeline::getCS(device, "dot_calc_points.cso");
	dot_ini = Pipeline::getCS(device, "dot_ini.cso");
	dot_calc = Pipeline::getCS(device, "dot_calc.cso");

	curve_draw.vs = Pipeline::getVS(device, "dot_vert.cso");
	curve_draw.ps = Pipeline::getPS(device, "dot_ps.cso");
	// AlphaBlend for the SDF antialiasing at each dot's edge, same as the other two renderers.
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});

	// One uint of atomically accumulated dot count; never resized.
	dot_counter.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(1u)));
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

	calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// World arc length only - the second channel of `distances` is always 0 and the scan sums it right
	// along with the first, harmlessly. Shared, unmodified SegmentedScan: see bezier_dots.h.
	scan.Scan(*distances, *curve_begins, total_points, context);

	ClearComputeBindings(context);
}

void BezierDotRenderer::CountDots(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(curves.size());

	const uint32_t zero = 0u;
	dot_counter->Upload(std::span<const uint32_t>{ &zero, 1 }, context);

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	bezier_data->Bind(ShaderStage::Compute, 0, context);        // t0
	distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);       // t2
	dot_counter->BindUnordered(0, context);                     // u0
	dot_ranges->BindUnordered(1, context);                      // u1

	dot_ini->Run({ (curve_count + 64u - 1u) / 64u, 1u, 1u }, context);

	ClearComputeBindings(context);

	const auto counter = dot_counter->Download<uint32_t>(context);
	dot_total = counter.empty() ? 0u : counter[0];

	const uint32_t required = next_pow2(std::max(dot_total, 1u));
	if (!dots || dots_allocated < required) {
		dots.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<DotSample>(required)));
		dots_allocated = required;
	}
}

void BezierDotRenderer::RunDotPlacementPass(GraphicsDeviceContext* context) {
	if (dot_total == 0 || !dots) return;

	const auto curve_count = static_cast<uint32_t>(curves.size());

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);          // b0
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

	if (UpdateBuffers(device, context)) need_recount = true;

	if (total_points < 2 || !calculated_points) return;

	UploadCameraData(view_proj, context);

	// Screen-space projection happens per-vertex in dot_vert.hlsl, straight from CalculatedPoints, so
	// unlike the patterned renderer nothing here depends on the camera except that final projection -
	// the point pass and the dot count are both pure world-space work. Still rerun every frame to
	// match the other two renderers' behaviour (see BezierSolidRenderer::Draw for the same note).
	RunPointPass(context);

	if (need_recount) {
		CountDots(device, context);
		need_recount = false;
	}

	RunDotPlacementPass(context);

	if (dot_total == 0 || !dots) return;

	// --- dot draw ---------------------------------------------------------------------
	curve_draw.Bind(context);

	calculated_points->BindOrdered(ShaderStage::Vertex, 0, context); // t0: points & packed colours
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);          // t4: point -> curve index
	curve_styles->Bind(ShaderStage::Vertex, 6, context);             // t6: width / caps / spacing
	dots->BindOrdered(ShaderStage::Vertex, 7, context);              // t7: per-dot bracketing samples + t

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(4, dot_total, 0, 0);

	ClearDrawBindings(context);
}
