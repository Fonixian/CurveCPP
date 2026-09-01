#include "bezier_simple.h"
#include "Include/Axodox.Storage.h"
#include <algorithm>
#include <cassert>
#include <limits>

using namespace Axodox::Graphics;
using namespace Axodox::Storage;
using namespace DirectX;

const auto bezier_curve_limit = std::numeric_limits<uint16_t>::max();

// Max point count for SegmentedScan scratch buffers.
constexpr uint32_t maxElementCount = 1'200'000u;

// Matches BezierCurveData in shaders/bezier_calc_points.hlsl, shaders/pattern_center_ini.hlsl
// and shaders/pattern_center_calc.hlsl.
struct UploadBezierData
{
	XMFLOAT3 P0;
	int first_index;
	XMFLOAT3 P1;
	int last_index;
	XMFLOAT3 P2;
	int color_begin;
	XMFLOAT3 P3;
	int color_end;
};

// Matches CurveStyle in shaders/bezier_vert.hlsl.
struct UploadCurveStyle
{
	float width;                // Half-width (radius) in pixels
	uint32_t style;             // PatternStyle
	float dash_length;          // Half-length of one dash, in pixels (ArcDash mode)
	float glyph_size;           // Glyph half-extent; ZERO in ArcDash mode
	uint32_t glyph_kind;        // GlyphKind
	uint32_t glyph_world_sized; // Non-zero: glyph_size is in world units
	float padding[2];
};

inline uint32_t PackFloat3ToR8G8B8A8(const DirectX::XMFLOAT3& color)
{
	float r = std::clamp(color.x, 0.0f, 1.0f);
	float g = std::clamp(color.y, 0.0f, 1.0f);
	float b = std::clamp(color.z, 0.0f, 1.0f);

	uint32_t ir = static_cast<uint32_t>(r * 255.0f + 0.5f);
	uint32_t ig = static_cast<uint32_t>(g * 255.0f + 0.5f);
	uint32_t ib = static_cast<uint32_t>(b * 255.0f + 0.5f);
	uint32_t ia = 255;

	return (ia << 24) | (ib << 16) | (ig << 8) | ir;
}

namespace
{
	// The compute stage keeps SRV and UAV bindings until they are explicitly replaced, and the
	// same buffers alternate between the two roles across passes (e.g. Distances is a UAV for the
	// scan and an SRV for pattern_center_ini). D3D11 silently unbinds one side of such a conflict,
	// so every pass clears the slots it is about to reuse instead of relying on that.
	constexpr uint32_t computeSrvSlots = 5u;
	constexpr uint32_t computeUavSlots = 5u;

	void ClearComputeBindings(GraphicsDeviceContext* context)
	{
		for (uint32_t slot = 0; slot < computeSrvSlots; ++slot)
			context->BindShaderResourceView(nullptr, ShaderStage::Compute, slot);
		for (uint32_t slot = 0; slot < computeUavSlots; ++slot)
			context->BindUnorderedAccessView(nullptr, slot);
	}

	// Same problem in the other direction: the draw leaves the compute-pass results bound as
	// vertex/pixel SRVs, which collides with next frame's UAV writes to those same buffers.
	constexpr uint32_t vertexSrvSlots = 8u;

	void ClearDrawBindings(GraphicsDeviceContext* context)
	{
		for (uint32_t slot = 0; slot < vertexSrvSlots; ++slot)
			context->BindShaderResourceView(nullptr, ShaderStage::Vertex, slot);
		context->BindShaderResourceView(nullptr, ShaderStage::Pixel, 1);
	}
}

BezierRenderer::BezierRenderer(const GraphicsDevice& device) : scan{ device, maxElementCount }
{
	calc_points = Pipeline::getCS(device, "bezier_calc_points.cso");
	pattern_ini = Pipeline::getCS(device, "pattern_center_ini.cso");
	pattern_calc = Pipeline::getCS(device, "pattern_center_calc.cso");
	pattern_glyph = Pipeline::getCS(device, "pattern_center_glyph.cso");

	// Curve draw pipeline
	curve_draw.vs = Pipeline::getVS(device, "bezier_vert.cso");
	curve_draw.ps = Pipeline::getPS(device, "bezier_ps.cso");
	// Use AlphaBlend for SDF antialiasing and dashed pattern gaps.
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
		});

	// Same vertex shader and same state - only the pixel shader differs. The vertex shader emits
	// the glyph interpolants unconditionally, and a pixel shader's input signature only has to be
	// a subset of the vertex shader's output, so both link against it.
	curve_draw_glyph.vs = curve_draw.vs;
	curve_draw_glyph.ps = Pipeline::getPS(device, "bezier_glyph_ps.cso");
	curve_draw_glyph.states = curve_draw.states;

	viewport_data = std::make_unique<ConstantBuffer>(device, camera_cb_data);
	dispatch_info = std::make_unique<ConstantBuffer>(device, DispatchInfoBuffer{});

	// One uint of atomically accumulated pattern count; never resized.
	pattern_counter.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(1u)));
}

// Screen-space arc length is recomputed every frame from this, so a viewport change needs no
// rebuild - it just has to land in the constant buffer before the next point pass.
void BezierRenderer::SetViewport(float width, float height)
{
	camera_cb_data.wh = { width, height };
}

void BezierRenderer::Add(const Bezier& curve)
{
	assert(curves.size() != bezier_curve_limit);
	assert(curve.resolution >= 2);
	curves.push_back(curve);
	need_resize = true;
}

void BezierRenderer::Clear()
{
	curves.clear();
	need_resize = true;
}

void BezierRenderer::SetPatternMode(PatternMode mode)
{
	if (pattern_mode == mode) return;
	pattern_mode = mode;
	// The pattern buffer's element type differs between modes, so this has to go through a
	// rebuild rather than a re-upload.
	need_resize = true;
}

void BezierRenderer::SetGlyph(GlyphKind kind, float size, bool worldSized)
{
	if (glyph_kind == kind && glyph_size == size && glyph_world_sized == worldSized) return;
	glyph_kind = kind;
	glyph_size = size;
	glyph_world_sized = worldSized;
	// Glyph appearance changes nothing about how many patterns exist or where their centers are,
	// so no rebuild and no readback - just the per-curve style buffer.
	styles_dirty = true;
}

void BezierRenderer::RefreshStyles(GraphicsDeviceContext* context)
{
	styles_dirty = false;
	if (curves.empty() || !curve_styles) return;

	const bool glyph = pattern_mode == PatternMode::Glyph;

	std::vector<UploadCurveStyle> style_data;
	style_data.reserve(curves.size());
	for (const auto& bez : curves)
	{
		style_data.push_back(UploadCurveStyle{
			bez.width,
			static_cast<uint32_t>(bez.pattern),
			bez.width * DashLengthPerWidth,
			// Zero in ArcDash mode. The vertex shader widens the triangle strip by this, so a
			// zero here means the arc-length path emits exactly the geometry it always did.
			glyph ? glyph_size : 0.0f,
			static_cast<uint32_t>(glyph_kind),
			(glyph && glyph_world_sized) ? 1u : 0u,
			{ 0.0f, 0.0f }
			});
	}

	curve_styles->Upload(std::span<const UploadCurveStyle>{ style_data }, context);
}

void BezierRenderer::AllocateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context)
{
	total = 0;
	for (const auto& bez : curves) total += bez.resolution;

	if (total <= 0)
	{
		bezier_data.reset();
		bezier_data_map.reset();
		curve_spacing.reset();
		curve_styles.reset();
		calculated_points.reset();
		distances.reset();
		distances_screen.reset();
		curve_begins.reset();
		pattern_ranges.reset();
		patterns.reset();
		pattern_total = 0;
		return;
	}

	// Build curve definitions, point-to-curve index mapping, and segmented scan reset flags.
	std::vector<UploadBezierData> upload_data;
	upload_data.reserve(curves.size());

	std::vector<float> spacing_data;
	spacing_data.reserve(curves.size());

	std::vector<uint16_t> index_map;
	index_map.reserve(static_cast<size_t>(total));

	// Pack 32 curve-begin flags per uint32.
	std::vector<uint32_t> curve_begin_bits((static_cast<size_t>(total) + 31u) / 32u, 0u);

	int current = 0;
	for (uint16_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex)
	{
		const Bezier& bez = curves[curveIndex];

		upload_data.push_back(UploadBezierData{
			bez.P0, current,
			bez.P1, current + bez.resolution - 1,
			bez.P2, static_cast<int>(PackFloat3ToR8G8B8A8(bez.C0)),
			bez.P3, static_cast<int>(PackFloat3ToR8G8B8A8(bez.C1))
			});

		spacing_data.push_back(bez.spacing);

		curve_begin_bits[current / 32] |= (1u << (current % 32));

		for (int i = 0; i < bez.resolution; ++i)
			index_map.push_back(curveIndex);

		current += bez.resolution;
	}
	bezier_data.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadBezierData>(std::span{ upload_data })));
	curve_spacing.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<float>(std::span{ spacing_data })));
	curve_begins.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::span{ curve_begin_bits })));

	// Created from a capacity rather than immutable data, so it lands in dynamic memory and
	// SetGlyph() can re-upload it without rebuilding anything else.
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadCurveStyle>(static_cast<uint32_t>(curves.size()))));
	RefreshStyles(context);

	// Pack two uint16 curve indices into each uint32 element. Both bezier_calc_points.hlsl and
	// bezier_vert.hlsl unpack this with UnpackCurveIndex().
	std::vector<uint32_t> packed_index_map;
	packed_index_map.reserve((index_map.size() + 1) / 2);
	for (size_t i = 0; i < index_map.size(); i += 2)
	{
		uint32_t lo = index_map[i];
		uint32_t hi = (i + 1 < index_map.size()) ? index_map[i + 1] : 0u;
		packed_index_map.push_back(lo | (hi << 16));
	}
	bezier_data_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::span{ packed_index_map })));

	calculated_points.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT4>(static_cast<uint32_t>(total))));
	distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(static_cast<uint32_t>(total))));
	distances_screen.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(static_cast<uint32_t>(total))));
	pattern_ranges.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMUINT2>(static_cast<uint32_t>(curves.size()))));

	// 'patterns' is deliberately left alone here - CountPatternCenters() grows it once the count
	// is known and reuses it when it is already big enough.
	pattern_total = 0;
}

void BezierRenderer::RunPointPass(GraphicsDeviceContext* context)
{
	// Step 1: sample points, colors, and per-segment world + screen lengths.
	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);   // b0
	bezier_data->Bind(ShaderStage::Compute, 0, context);     // t0
	bezier_data_map->Bind(ShaderStage::Compute, 1, context); // t1
	calculated_points->BindUnordered(0, context);            // u0
	distances->BindUnordered(1, context);                    // u1
	distances_screen->BindUnordered(2, context);             // u2

	calc_points->Run({ (static_cast<uint32_t>(total) + 256u - 1u) / 256u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// Step 2: turn the per-segment lengths into cumulative arc length, per curve. The scan is
	// exclusive, so Distances[lastIndex of a curve] is exactly that curve's total arc length.
	scan.Scan(*distances, *curve_begins, static_cast<uint32_t>(total), context);
	scan.Scan(*distances_screen, *curve_begins, static_cast<uint32_t>(total), context);

	ClearComputeBindings(context);
}

void BezierRenderer::CountPatternCenters(const GraphicsDevice& device, GraphicsDeviceContext* context)
{
	const auto curveCount = static_cast<uint32_t>(curves.size());

	dispatch_info->Upload(DispatchInfoBuffer{ curveCount }, context);

	// InterlockedAdd accumulates into this, so it has to start from zero every time.
	const uint32_t zero = 0u;
	pattern_counter->Upload(std::span<const uint32_t>{ &zero, 1 }, context);

	ClearComputeBindings(context);

	// Step 3: dotCount per curve, and a reserved [first, count) range inside one shared buffer.
	dispatch_info->Bind(ShaderStage::Compute, 0, context);              // b0
	bezier_data->Bind(ShaderStage::Compute, 0, context);                // t0
	distances->BindOrdered(ShaderStage::Compute, 1, context);           // t1
	curve_spacing->Bind(ShaderStage::Compute, 2, context);              // t2
	pattern_counter->BindUnordered(0, context);                         // u0
	pattern_ranges->BindUnordered(1, context);                          // u1

	pattern_ini->Run({ (curveCount + 64u - 1u) / 64u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// Blocking readback. Only acceptable because pattern counts depend on world arc length and
	// world spacing alone - both camera independent - so this runs on rebuild, not every frame.
	const auto counter = pattern_counter->Download<uint32_t>(context);
	pattern_total = counter.empty() ? 0u : counter[0];

	// Keep the buffer when it is already large enough - the animated example rebuilds every
	// frame, and reallocating a GPU resource per frame is pure churn. The element type differs
	// between modes, so a mode switch always forces a fresh allocation.
	const uint32_t required = std::max(pattern_total, 1u);
	if (!patterns || patterns_layout != pattern_mode || patterns->Capacity() < required)
	{
		if (pattern_mode == PatternMode::Glyph)
			patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT4>(required)));
		else
			patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));

		patterns_layout = pattern_mode;
	}
}

void BezierRenderer::RunPatternPass(GraphicsDeviceContext* context)
{
	if (pattern_total == 0 || !patterns) return;

	const auto curveCount = static_cast<uint32_t>(curves.size());

	ClearComputeBindings(context);

	// Step 4: both modes locate each pattern center's world arc length in the world prefix sum.
	// They differ in what they read back out at that spot.
	dispatch_info->Bind(ShaderStage::Compute, 0, context);              // b0
	bezier_data->Bind(ShaderStage::Compute, 0, context);                // t0
	distances->BindOrdered(ShaderStage::Compute, 1, context);           // t1

	if (pattern_mode == PatternMode::Glyph)
	{
		// ...the world position, projected, plus the screen tangent there.
		viewport_data->Bind(ShaderStage::Compute, 1, context);          // b1
		calculated_points->BindOrdered(ShaderStage::Compute, 2, context); // t2
	}
	else
	{
		// ...the screen prefix sum, lerped by the same factor.
		distances_screen->BindOrdered(ShaderStage::Compute, 2, context); // t2
	}

	curve_spacing->Bind(ShaderStage::Compute, 3, context);              // t3
	pattern_ranges->BindOrdered(ShaderStage::Compute, 4, context);      // t4
	patterns->BindUnordered(0, context);                                // u0

	// 8 threads per curve on x, 8 curves per group on y.
	const DirectX::XMUINT3 groups{ 1u, (curveCount + 8u - 1u) / 8u, 1u };
	if (pattern_mode == PatternMode::Glyph) pattern_glyph->Run(groups, context);
	else                                    pattern_calc->Run(groups, context);

	ClearComputeBindings(context);
}

void BezierRenderer::Draw(const GraphicsDevice& device, const DirectX::XMMATRIX& view_proj)
{
	// Cast constness to get the immediate context.
	auto* context = const_cast<GraphicsDevice&>(device).ImmediateContext();

	DirectX::XMStoreFloat4x4(&camera_cb_data.VP, DirectX::XMMatrixTranspose(view_proj));
	viewport_data->Upload(camera_cb_data, context);

	if (need_resize)
	{
		AllocateBuffers(device, context);
		need_resize = false;
		need_recount = true;
	}
	else if (styles_dirty)
	{
		RefreshStyles(context);
	}

	if (total < 2 || !calculated_points) return;

	// Screen-space arc length depends on the camera, so steps 1-2 and step 4 rerun every frame.
	RunPointPass(context);

	if (need_recount)
	{
		// Runs after the point pass so the world prefix sum it reads is already valid.
		CountPatternCenters(device, context);
		need_recount = false;
	}

	RunPatternPass(context);

	// --- curve-body draw --------------------------------------------------------------
	// Same vertex shader and same vertex bindings either way; only the pixel shader and the
	// meaning of the pattern buffer change.
	Pipeline& pipeline = (pattern_mode == PatternMode::Glyph) ? curve_draw_glyph : curve_draw;
	pipeline.Bind(context);

	calculated_points->BindOrdered(ShaderStage::Vertex, 0, context);  // t0: calculated points & colors
	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);       // t1: curve boundary flags
	distances->BindOrdered(ShaderStage::Vertex, 2, context);          // t2: cumulative WORLD arc length
	curve_spacing->Bind(ShaderStage::Vertex, 3, context);             // t3: world spacing per curve
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);           // t4: packed point -> curve index
	distances_screen->BindOrdered(ShaderStage::Vertex, 5, context);   // t5: cumulative SCREEN arc length
	pattern_ranges->BindOrdered(ShaderStage::Vertex, 6, context);     // t6: pattern range per curve
	curve_styles->Bind(ShaderStage::Vertex, 7, context);              // t7: width / style / dash length

	// t1: ArcDash reads this as StructuredBuffer<float>, Glyph as StructuredBuffer<float4>.
	patterns->BindOrdered(ShaderStage::Pixel, 1, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->get()->DrawInstanced(5, static_cast<uint32_t>(total) - 1u, 0, 0);

	// Release the SRVs so next frame's compute passes can bind the same buffers as UAVs.
	ClearDrawBindings(context);
}

inline DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t) {
	DirectX::XMVECTOR va = DirectX::XMLoadFloat3(&a);
	DirectX::XMVECTOR vb = DirectX::XMLoadFloat3(&b);
	DirectX::XMVECTOR res = DirectX::XMVectorLerp(va, vb, t);
	DirectX::XMFLOAT3 out;
	DirectX::XMStoreFloat3(&out, res);
	return out;
}

Bezier linear(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1,
	DirectX::XMFLOAT3 C0, DirectX::XMFLOAT3 C1,
	float width, PatternStyle pattern, float spacing, int resolution)
{
	DirectX::XMFLOAT3 cp0 = P0;
	DirectX::XMFLOAT3 cp1 = LerpFloat3(P0, P1, 1.0f / 3.0f);
	DirectX::XMFLOAT3 cp2 = LerpFloat3(P0, P1, 2.0f / 3.0f);
	DirectX::XMFLOAT3 cp3 = P1;
	return { cp0, cp1, cp2, cp3, C0, C1, width, pattern, spacing, resolution };
}

Bezier quadratic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2,
	DirectX::XMFLOAT3 C0, DirectX::XMFLOAT3 C1,
	float width, PatternStyle pattern, float spacing, int resolution)
{
	DirectX::XMFLOAT3 cp0 = P0;
	DirectX::XMFLOAT3 cp1 = LerpFloat3(P0, P1, 2.0f / 3.0f);
	DirectX::XMFLOAT3 cp2 = LerpFloat3(P1, P2, 1.0f / 3.0f);
	DirectX::XMFLOAT3 cp3 = P2;
	return { cp0, cp1, cp2, cp3, C0, C1, width, pattern, spacing, resolution };
}

Bezier cubic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2, DirectX::XMFLOAT3 P3,
	DirectX::XMFLOAT3 C0, DirectX::XMFLOAT3 C1,
	float width, PatternStyle pattern, float spacing, int resolution)
{
	return { P0, P1, P2, P3, C0, C1, width, pattern, spacing ,resolution };
}
