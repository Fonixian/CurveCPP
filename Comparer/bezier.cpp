#include "bezier.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

using namespace Axodox::Graphics;
using namespace DirectX;

namespace curve
{
	namespace
	{
		// Max point count for SegmentedScan scratch buffers.
		constexpr uint32_t maxElementCount = 1'200'000u;

		unsigned next_pow2(unsigned x)
		{
			if (x <= 1u) return 1u;
			return 1u << (std::numeric_limits<unsigned>::digits - std::countl_zero(x - 1u));
		}

		// Matches BezierCurveData in shaders/bezier/curve_calc_points.hlsl,
		// curve_pattern_ini.hlsl and curve_pattern_calc.hlsl. 80 bytes.
		struct UploadBezierData
		{
			XMFLOAT3 P0;
			int32_t  first_index;
			XMFLOAT3 P1;
			int32_t  last_index;
			XMFLOAT3 P2;
			uint32_t color_begin;
			XMFLOAT3 P3;
			uint32_t color_end;
			float    min_height;
			float    max_height;
			float    padding[2];
		};
		static_assert(sizeof(UploadBezierData) == 80);

		// Matches CurveStyle in shaders/bezier/curve_vert.hlsl,
		// curve_pattern_ini.hlsl and curve_pattern_calc.hlsl. 32 bytes.
		struct UploadCurveStyle
		{
			float    width;       // Half-width (radius) in pixels
			uint32_t cap;         // CurveCap  - uploaded, not implemented yet
			uint32_t join;        // CurveJoin - uploaded, not implemented yet
			uint32_t pattern;     // CurvePattern
			float    spacing;     // World-space arc length between pattern centers
			float    dash_length; // Half-length of one dash/dot, in pixels
			float    padding[2];
		};
		static_assert(sizeof(UploadCurveStyle) == 32);

		uint32_t PackFloat3ToR8G8B8A8(const XMFLOAT3& color)
		{
			const float r = std::clamp(color.x, 0.0f, 1.0f);
			const float g = std::clamp(color.y, 0.0f, 1.0f);
			const float b = std::clamp(color.z, 0.0f, 1.0f);

			const uint32_t ir = static_cast<uint32_t>(r * 255.0f + 0.5f);
			const uint32_t ig = static_cast<uint32_t>(g * 255.0f + 0.5f);
			const uint32_t ib = static_cast<uint32_t>(b * 255.0f + 0.5f);
			const uint32_t ia = 255u;

			return (ia << 24) | (ib << 16) | (ig << 8) | ir;
		}

		XMFLOAT3 LerpFloat3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
		{
			XMFLOAT3 out;
			XMStoreFloat3(&out, XMVectorLerp(XMLoadFloat3(&a), XMLoadFloat3(&b), t));
			return out;
		}

		// Curves are stored in the degree they were given; this is the only place they become
		// cubic. Degree elevation is exact, so the sampled points are identical either way.
		void ToCubic(const BezierData& source, XMFLOAT3& p0, XMFLOAT3& p1, XMFLOAT3& p2, XMFLOAT3& p3)
		{
			switch (source.power())
			{
			case 1: // linear: P0 -> P1
				p0 = source.P0;
				p1 = LerpFloat3(source.P0, source.P1, 1.0f / 3.0f);
				p2 = LerpFloat3(source.P0, source.P1, 2.0f / 3.0f);
				p3 = source.P1;
				break;

			case 2: // quadratic: P0, P1, P2
				p0 = source.P0;
				p1 = LerpFloat3(source.P0, source.P1, 2.0f / 3.0f);
				p2 = LerpFloat3(source.P1, source.P2, 1.0f / 3.0f);
				p3 = source.P2;
				break;

			default: // cubic, or degenerate - pass through
				p0 = source.P0;
				p1 = source.P1;
				p2 = source.P2;
				p3 = source.P3;
				break;
			}
		}

		// The compute stage keeps SRV and UAV bindings until they are explicitly replaced, and the
		// same buffers alternate between the two roles across passes (e.g. Distances is a UAV for
		// the scan and an SRV for curve_pattern_ini). D3D11 silently unbinds one side of such a
		// conflict, so every pass clears the slots it is about to reuse instead of relying on that.
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

	// --- BezierData ----------------------------------------------------------------------

	bool BezierData::empty() const
	{
		return resolution == 0;
	}

	void BezierData::clear()
	{
		resolution = 0;
	}

	int BezierData::power() const
	{
		return bezier_power;
		const auto is_nan = [](const XMFLOAT3& v) { return std::isnan(v.x) || std::isnan(v.y) || std::isnan(v.z); };
		if (!is_nan(P0) && !is_nan(P1)) {
			if (!is_nan(P2)) {
				if (!is_nan(P3)) return 3;
				return 2;
			}
			return 1;
		}
		return -1;
	}

	BezierData Linear(XMFLOAT3 P0, XMFLOAT3 P1)
	{
		BezierData result;
		result.P0 = P0;
		result.P1 = P1;
		result.P2 = UnusedPoint;
		result.P3 = UnusedPoint;
		return result;
	}

	BezierData Quadratic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2)
	{
		BezierData result;
		result.P0 = P0;
		result.P1 = P1;
		result.P2 = P2;
		result.P3 = UnusedPoint;
		return result;
	}

	BezierData Cubic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2, XMFLOAT3 P3)
	{
		BezierData result;
		result.P0 = P0;
		result.P1 = P1;
		result.P2 = P2;
		result.P3 = P3;
		return result;
	}

	// --- BezierRenderer ------------------------------------------------------------------

	BezierRenderer::BezierRenderer(const GraphicsDevice& device) : scan{ device, maxElementCount }
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

		viewport_data = std::make_unique<ConstantBuffer>(device, camera_cb_data);

		// One uint of atomically accumulated pattern count; never resized.
		pattern_counter.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(1u)));
	}

	void BezierRenderer::SetViewport(float width, float height)
	{
		camera_cb_data.wh = { width, height };
	}

	BezierCurve BezierRenderer::Add(const BezierData& curve)
	{
		assert(curve.resolution >= 2u);
		assert(curve.power() >= 1);

		const size_t index = curves.size();
		curves.push_back(curve);
		need_resize = true;
		return BezierCurve{ this, index };
	}

	BezierCurve BezierRenderer::At(size_t index)
	{
		assert(index < curves.size());
		return BezierCurve{ this, index };
	}

	void BezierRenderer::AllocateBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context)
	{
		total_points = 0;
		for (const auto& bez : curves) total_points += bez.resolution;

		const auto curve_count = static_cast<uint32_t>(curves.size());
		if (total_points < 2u || curve_count == 0u)
		{
			total_points = 0;
			pattern_total = 0;
			return;
		}

		// Buffers are grown to the next power of two and reused. Curves are never removed, so
		// these only ever grow - shrinking would just churn GPU resources.
		const uint32_t points_required = next_pow2(total_points);
		const uint32_t curves_required = next_pow2(curve_count);

		if (points_allocated < points_required)
		{
			calculated_points.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT4>(points_required)));
			distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT2>(points_required)));
			//distances_screen.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT2>(points_required)));
			curve_begins.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::max((points_required + 31u) / 32u, 1u))));
			bezier_data_map.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(points_required)));
			points_allocated = points_required;
		}

		if (curves_allocated < curves_required)
		{
			bezier_data.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadBezierData>(curves_required)));
			curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadCurveStyle>(curves_required)));
			pattern_ranges.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<XMUINT2>(curves_required)));
			curves_allocated = curves_required;
		}

		// --- sample layout ------------------------------------------------------------
		// One uint32 curve index per point (this used to be two packed uint16s).
		std::vector<uint32_t> index_map;
		index_map.reserve(total_points);

		// 32 curve-begin flags per uint32. curve_begins is an RWStructuredBuffer, which uses
		// default usage, so Upload() goes through UpdateSubresource() with a null box - that reads
		// the resource's full ByteWidth from the source pointer. The vector therefore has to cover
		// the whole allocation, not just the live part.
		std::vector<uint32_t> curve_begin_bits(std::max((points_allocated + 31u) / 32u, 1u), 0u);

		uint32_t current = 0;
		for (uint32_t curveIndex = 0; curveIndex < curve_count; ++curveIndex)
		{
			const BezierData& bez = curves[curveIndex];

			curve_begin_bits[current / 32u] |= (1u << (current % 32u));

			for (unsigned i = 0; i < bez.resolution; ++i)
				index_map.push_back(curveIndex);

			current += bez.resolution;
		}

		bezier_data_map->Upload(std::span<const uint32_t>{ index_map }, context);
		curve_begins->Upload(std::span<const uint32_t>{ curve_begin_bits }, context);

		// The per-curve definitions carry first/last point index, so they depend on the layout too.
		UploadCurveData(context);
	}

	void BezierRenderer::UploadCurveData(GraphicsDeviceContext* context)
	{
		if (curves.empty() || !bezier_data || !curve_styles) return;

		std::vector<UploadBezierData> upload_data;
		upload_data.reserve(curves.size());

		std::vector<UploadCurveStyle> style_data;
		style_data.reserve(curves.size());

		int32_t current = 0;
		for (const auto& bez : curves)
		{
			XMFLOAT3 p0, p1, p2, p3;
			ToCubic(bez, p0, p1, p2, p3);

			const int32_t first = current;
			const int32_t last = current + static_cast<int32_t>(bez.resolution) - 1;

			upload_data.push_back(UploadBezierData{
				p0, first,
				p1, last,
				p2, PackFloat3ToR8G8B8A8(bez.C0),
				p3, PackFloat3ToR8G8B8A8(bez.C1),
				bez.min_height, bez.max_height,
				{ 0.0f, 0.0f }
				});

			// Half-length of one mark along the curve, in pixels. Solid never reads it.
			float dash_length = 0.0f;
			switch (bez.pattern)
			{
			case CurvePattern::Dash: dash_length = bez.width * DashLengthPerWidth; break;
			case CurvePattern::Dot:  dash_length = bez.width * DotLengthPerWidth;  break;
			default: break;
			}

			style_data.push_back(UploadCurveStyle{
				bez.width,
				static_cast<uint32_t>(bez.cap),
				static_cast<uint32_t>(bez.join),
				static_cast<uint32_t>(bez.pattern),
				bez.spacing,
				dash_length,
				{ 0.0f, 0.0f }
				});

			current += static_cast<int32_t>(bez.resolution);
		}

		// Both are plain StructuredBuffers created from a capacity, so they are dynamic and a
		// partial Map/memcpy of just the live prefix is safe.
		bezier_data->Upload(std::span<const UploadBezierData>{ upload_data }, context);
		curve_styles->Upload(std::span<const UploadCurveStyle>{ style_data }, context);
	}

	void BezierRenderer::RunPointPass(GraphicsDeviceContext* context)
	{
		// Step 1: sample points, colours, and per-segment world + screen lengths.
		ClearComputeBindings(context);

		viewport_data->Bind(ShaderStage::Compute, 0, context);   // b0
		bezier_data->Bind(ShaderStage::Compute, 0, context);     // t0
		bezier_data_map->Bind(ShaderStage::Compute, 1, context); // t1
		calculated_points->BindUnordered(0, context);            // u0
		distances->BindUnordered(1, context);                    // u1
		//distances_screen->BindUnordered(2, context);             // u2

		calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);

		ClearComputeBindings(context);

		// Step 2: turn the per-segment lengths into cumulative arc length, per curve. The scan is
		// exclusive, so Distances[lastIndex of a curve] is exactly that curve's total arc length.
		scan.Scan(*distances, *curve_begins, total_points, context);
		//scan.Scan(*distances_screen, *curve_begins, total_points, context);

		ClearComputeBindings(context);
	}

	void BezierRenderer::CountPatternCenters(const GraphicsDevice& device, GraphicsDeviceContext* context)
	{
		const auto curve_count = static_cast<uint32_t>(curves.size());

		// InterlockedAdd accumulates into this, so it has to start from zero every time. Capacity
		// is exactly one uint, so the full-ByteWidth UpdateSubresource read is in bounds.
		const uint32_t zero = 0u;
		pattern_counter->Upload(std::span<const uint32_t>{ &zero, 1 }, context);

		ClearComputeBindings(context);

		// Step 3: pattern count per curve, and a reserved [first, count) range in a shared buffer.
		viewport_data->Bind(ShaderStage::Compute, 0, context);      // b0
		bezier_data->Bind(ShaderStage::Compute, 0, context);        // t0
		distances->BindOrdered(ShaderStage::Compute, 1, context);   // t1
		curve_styles->Bind(ShaderStage::Compute, 2, context);       // t2
		pattern_counter->BindUnordered(0, context);                 // u0
		pattern_ranges->BindUnordered(1, context);                  // u1

		pattern_ini->Run({ (curve_count + 64u - 1u) / 64u, 1u, 1u }, context);

		ClearComputeBindings(context);

		// Blocking readback. Only acceptable because pattern counts depend on world arc length and
		// world spacing alone - both camera independent - so this runs on a change, not per frame.
		const auto counter = pattern_counter->Download<uint32_t>(context);
		pattern_total = counter.empty() ? 0u : counter[0];

		const uint32_t required = next_pow2(std::max(pattern_total, 1u));
		if (!patterns || patterns_allocated < required)
		{
			patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));
			patterns_allocated = required;
		}
	}

	void BezierRenderer::RunPatternPass(GraphicsDeviceContext* context)
	{
		if (pattern_total == 0 || !patterns) return;

		const auto curve_count = static_cast<uint32_t>(curves.size());

		ClearComputeBindings(context);

		// Step 4: locate each pattern center's world arc length in the world prefix sum, then read
		// the screen prefix sum back out at the same spot.
		viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
		bezier_data->Bind(ShaderStage::Compute, 0, context);              // t0
		distances->BindOrdered(ShaderStage::Compute, 1, context);         // t1
		//distances_screen->BindOrdered(ShaderStage::Compute, 2, context);  // t2
		curve_styles->Bind(ShaderStage::Compute, 3, context);             // t3
		pattern_ranges->BindOrdered(ShaderStage::Compute, 4, context);    // t4
		patterns->BindUnordered(0, context);                              // u0

		// 8 threads per curve on x, 8 curves per group on y.
		pattern_calc->Run({ 1u, (curve_count + 8u - 1u) / 8u, 1u }, context);

		ClearComputeBindings(context);
	}

	void BezierRenderer::Draw(const GraphicsDevice& device, const DirectX::XMMATRIX& view_proj)
	{
		// Cast constness to get the immediate context.
		auto* context = const_cast<GraphicsDevice&>(device).ImmediateContext();

		if (need_resize)
		{
			AllocateBuffers(device, context);
			need_resize = false;
			need_upload = false;
			need_recount = true;
		}
		else if (need_upload)
		{
			UploadCurveData(context);
			need_upload = false;
			need_recount = true;
		}

		if (total_points < 2 || !calculated_points) return;

		XMStoreFloat4x4(&camera_cb_data.VP, XMMatrixTranspose(view_proj));
		camera_cb_data.TotalPointCount = total_points;
		camera_cb_data.TotalCurveCount = static_cast<uint32_t>(curves.size());
		viewport_data->Upload(camera_cb_data, context);

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
		curve_draw.Bind(context);

		calculated_points->BindOrdered(ShaderStage::Vertex, 0, context); // t0: points & packed colours
		curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);      // t1: curve boundary flags
		distances->BindOrdered(ShaderStage::Vertex, 2, context);         // t2: cumulative WORLD arc length
		//distances_screen->BindOrdered(ShaderStage::Vertex, 3, context);  // t3: cumulative SCREEN arc length
		bezier_data_map->Bind(ShaderStage::Vertex, 4, context);          // t4: point -> curve index
		pattern_ranges->BindOrdered(ShaderStage::Vertex, 5, context);    // t5: pattern range per curve
		curve_styles->Bind(ShaderStage::Vertex, 6, context);             // t6: width / cap / join / pattern / spacing

		if (patterns) patterns->BindOrdered(ShaderStage::Pixel, 1, context); // t1: screen arc length per pattern center

		viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
		viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

		context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->get()->DrawInstanced(5, total_points - 1u, 0, 0);

		// Release the SRVs so next frame's compute passes can bind the same buffers as UAVs.
		ClearDrawBindings(context);
	}
}
