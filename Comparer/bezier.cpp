#include "bezier.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

constexpr uint32_t maxElementCount = 1'200'000u;

// Width / cap / join / pattern / spacing, as curve_common.hlsli's CurveStyle reads it.
struct UploadCurveStyle {
	float    width;
	uint32_t cap;
	uint32_t join;
	uint32_t pattern;
	float    spacing;
	float    dash_length;
	float    padding[2];
};

// --- BezierRenderer ----------------------------------------------------------------------------

BezierRenderer::BezierRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device), scan{ device, maxElementCount }
{
	calc_points = Pipeline::getCS(device, "curve_calc_points.cso");
	pattern_ini = Pipeline::getCS(device, "curve_pattern_ini.cso");
	pattern_calc = Pipeline::getCS(device, "curve_pattern_calc.cso");

	curve_draw.vs = Pipeline::getVS(device, "VertexShader.cso");
	curve_draw.ps = Pipeline::getPS(device, "curve_ps.cso");
	// AlphaBlend for the SDF antialiasing and the pattern gaps.
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});

	// One uint of atomically accumulated pattern count; never resized.
	pattern_counter.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(1u)));
}

void BezierRenderer::AllocatePointBuffers(const GraphicsDevice& device, uint32_t points_required) {
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT2>(points_required)));
}

void BezierRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	pattern_ranges.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMUINT2>(curves_required)));
}

void BezierRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadCurveStyle>(curves_required)));
}

void BezierRenderer::UploadStyles(GraphicsDeviceContext* context) {
	std::vector<UploadCurveStyle> style_data;
	style_data.reserve(curves.size());

	for (const auto& bez : curves) {
		// Half-length of one mark along the curve, in pixels. Solid never reads it.
		float dash_length = 0.0f;
		switch (bez.pattern) {
			case CurvePattern::Dash: dash_length = bez.width * DashLengthPerWidth; break;
			case CurvePattern::Dot:  dash_length = bez.width * DotLengthPerWidth;  break;
			default: break;
		}

		style_data.push_back(UploadCurveStyle{
			bez.width,
			static_cast<uint32_t>(bez.cap_front),
			static_cast<uint32_t>(bez.join),
			static_cast<uint32_t>(bez.pattern),
			bez.spacing,
			dash_length,
			{ 0.0f, 0.0f }
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

	calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);

	ClearComputeBindings(context);

	scan.Scan(*distances, *curve_begins, total_points, context);

	ClearComputeBindings(context);
}

void BezierRenderer::CountPatternCenters(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	const auto curve_count = static_cast<uint32_t>(curves.size());

	const uint32_t zero = 0u;
	pattern_counter->Upload(std::span<const uint32_t>{ &zero, 1 }, context);

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
	bezier_data->Bind(ShaderStage::Compute, 0, context);        // t0
	distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1
	curve_styles->Bind(ShaderStage::Compute, 2, context);       // t2
	pattern_counter->BindUnordered(0, context);                 // u0
	pattern_ranges->BindUnordered(1, context);                  // u1

	pattern_ini->Run({ (curve_count + 64u - 1u) / 64u, 1u, 1u }, context);

	ClearComputeBindings(context);

	const auto counter = pattern_counter->Download<uint32_t>(context);
	pattern_total = counter.empty() ? 0u : counter[0];

	const uint32_t required = next_pow2(std::max(pattern_total, 1u));
	if (!patterns || patterns_allocated < required) {
		patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));
		patterns_allocated = required;
	}
}

void BezierRenderer::RunPatternPass(GraphicsDeviceContext* context) {
	if (pattern_total == 0 || !patterns) return;

	const auto curve_count = static_cast<uint32_t>(curves.size());

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
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

	if (UpdateBuffers(device, context)) need_recount = true;

	if (total_points < 2 || !calculated_points) return;

	UploadCameraData(view_proj, context);

	// Screen-space arc length depends on the camera, so the point pass and the pattern pass rerun
	// every frame. Only the centre COUNT is camera-independent.
	RunPointPass(context);

	if (need_recount) {
		// Runs after the point pass so the world prefix sum it reads is already valid.
		CountPatternCenters(device, context);
		need_recount = false;
	}

	RunPatternPass(context);

	// --- curve-body draw --------------------------------------------------------------
	curve_draw.Bind(context);

	calculated_points->BindOrdered(ShaderStage::Vertex, 0, context); // t0: points & packed colours
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);      // t1: curve boundary flags
	distances->BindOrdered(ShaderStage::Vertex, 2, context);         // t2: cumulative (world, screen) arc length
	bezier_data->Bind(ShaderStage::Vertex, 3, context);              // t3: curve definitions
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);          // t4: point -> curve index
	pattern_ranges->BindOrdered(ShaderStage::Vertex, 5, context);    // t5: pattern range per curve
	curve_styles->Bind(ShaderStage::Vertex, 6, context);             // t6: width / cap / join / pattern / spacing

	if (patterns) patterns->BindOrdered(ShaderStage::Pixel, 1, context); // t1: screen arc length per pattern center

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, total_points - 1u, 0, 0);

	ClearDrawBindings(context);
}
