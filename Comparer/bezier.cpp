#include "bezier.h"
#include <algorithm>
#include <iterator>

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

namespace {
	struct Double3 { double x, y, z; };

	Double3 ToDouble3(const XMFLOAT3& p) { return { p.x, p.y, p.z }; }

	// (1 - t) a + t b rather than a + t (b - a): exactly a at t = 0 and exactly b at t = 1.
	Double3 Lerp(const Double3& a, const Double3& b, double t) {
		const double s = 1.0 - t;
		return { s * a.x + t * b.x, s * a.y + t * b.y, s * a.z + t * b.z };
	}

	// The cubic's blossom B(u, v, w): de Casteljau with a different parameter per level. The
	// sub-curve over [a, b] has the control points B(a,a,a), B(a,a,b), B(a,b,b), B(b,b,b), so the
	// joint between two neighbouring pieces, B(b,b,b), is the same arithmetic on the same inputs
	// from both sides - bit-identical - and B(0,0,0) / B(1,1,1) are exactly P0 / P3.
	Double3 Blossom(const Double3 (&p)[4], double u, double v, double w) {
		const Double3 a0 = Lerp(p[0], p[1], u), a1 = Lerp(p[1], p[2], u), a2 = Lerp(p[2], p[3], u);
		const Double3 b0 = Lerp(a0, a1, v), b1 = Lerp(a1, a2, v);
		return Lerp(b0, b1, w);
	}

	XMFLOAT3 ToFloat3(const Double3& p) { return { float(p.x), float(p.y), float(p.z) }; }
}

uint32_t BezierRenderer::PieceCount(unsigned resolution) {
	return std::max((resolution + PieceSamples - 1u) >> PieceSampleShift, 1u);
}

// --- BezierRenderer ----------------------------------------------------------------------------

BezierRenderer::BezierRenderer(const GraphicsDevice& device)
	// offset_scan runs over curve counts, and a curve is worth at least one point, so maxElementCount
	// bounds the curve count too - no separate cap, and its buffers cost a few KB at that size.
	: BezierRendererBase(device),
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
}

void BezierRenderer::AllocatePointBuffers(const GraphicsDevice& device, uint32_t points_required) {
	// Two float buffers rather than one of float2: same bytes, but each pass loads only the channel
	// it actually reads, and each is scanned by its own SegmentedScan. See the note in bezier.h.
	world_distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(points_required)));
	screen_distances.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(points_required)));
}

void BezierRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	// `curves_required` counts PIECES here - see LayoutBuffers.
	piece_control_points.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT3>(curves_required * 4u)));
	piece_colors.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadPieceColor>(curves_required)));
	curve_chains.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadChainPieces>(curves_required)));

	// One element longer than the piece count on purpose: the scan parks the grand total in the slot
	// just past the last piece, and a structured-buffer UAV drops an out-of-range store without
	// complaining. curve_pattern_calc reads [i + 1] for its count and curve_vs reads [last piece of
	// the chain + 1] as the end of the chain's slot range, so the slot is live, not slack.
	pattern_offsets.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required + 1u)));
}

void BezierRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required) {
	curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<UploadPatternStyle>(curves_required)));
}

void BezierRenderer::LayoutBuffers(const GraphicsDevice& device, GraphicsDeviceContext* context) {
	const size_t curve_count = curves.size();

	piece_first.resize(curve_count + 1u);
	uint32_t pieces = 0;
	for (size_t curveIndex = 0; curveIndex < curve_count; ++curveIndex) {
		piece_first[curveIndex] = pieces;
		pieces += PieceCount(curves[curveIndex].resolution);
	}
	piece_first[curve_count] = pieces;
	piece_total = pieces;

	// 64 samples per piece, the last one the joint duplicate - see bezier.h.
	total_points = pieces << PieceSampleShift;
	if (pieces == 0u) {
		total_points = 0;
		pattern_upper_bound = 0;
		return;
	}
	assert(total_points <= maxElementCount && "more samples than the scans were built for");

	// Grow when full, shrink once a quarter or less is in use - see FitCapacity().
	const uint32_t points_required = FitCapacity(points_allocated, total_points);
	const uint32_t curves_required = FitCapacity(curves_allocated, pieces);

	if (points_allocated != points_required) {
		curve_begins.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(std::max((points_required + 31u) / 32u, 1u))));
		AllocatePointBuffers(device, points_required);
		points_allocated = points_required;
	}

	if (curves_allocated != curves_required) {
		AllocateStyleBuffer(device, curves_required);
		AllocateCurveBuffers(device, curves_required);
		curves_allocated = curves_required;
	}

	// One begin bit per chain, at the first sample of the chain's first piece - always a multiple of
	// 64, so always bit 0 of an even word. Sized to the whole allocation: curve_begins is an RW buffer,
	// whose Upload reads its full ByteWidth from the source.
	begin_scratch.assign(std::max((points_allocated + 31u) / 32u, 1u), 0u);
	for (size_t curveIndex = 0; curveIndex < curve_count; ++curveIndex) {
		if (!IsChainStart(curveIndex)) continue;
		const uint32_t sample = piece_first[curveIndex] << PieceSampleShift;
		begin_scratch[sample / 32u] |= 1u << (sample % 32u);
	}
	curve_begins->Upload(std::span<const uint32_t>{ begin_scratch }, context);

	// Everything: a buffer that was just (re)allocated holds nothing yet.
	UploadCurves(context, DirtyAll);
}

void BezierRenderer::UploadStyles(GraphicsDeviceContext* context) {
	style_scratch.resize(piece_total);
	for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
		const BezierData& bez = curves[curveIndex];
		const UploadPatternStyle style{ PackWidthCapCapJoin(bez), bez.spacing, bez.dash_length };
		for (uint32_t piece = piece_first[curveIndex]; piece < piece_first[curveIndex + 1u]; ++piece)
			style_scratch[piece] = style;
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

// Each buffer is rewritten whole when its part is dirty - StructuredBuffer::Upload maps with
// WRITE_DISCARD, so a buffer is either skipped entirely or refilled for every live piece.
void BezierRenderer::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	if (curves.empty() || piece_total == 0u || !piece_control_points) return;

	if (parts & DirtyPositions) {
		control_point_scratch.resize(size_t(piece_total) * 4u);

		[[maybe_unused]] XMFLOAT3 previous_end = {}; // P3 of the curve before, for the shared-endpoint check
		for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
			XMFLOAT3 cubic[4];
			ToCubic(curves[curveIndex], cubic[0], cubic[1], cubic[2], cubic[3]);

			assert((IsChainStart(curveIndex) || EndpointsMeet(previous_end, cubic[0])) &&
				"merge_with_previous on a curve that does not start where the previous one ends");
			previous_end = cubic[3];

			const uint32_t first = piece_first[curveIndex];
			const uint32_t count = piece_first[curveIndex + 1u] - first;
			XMFLOAT3* out = &control_point_scratch[size_t(first) * 4u];

			if (count == 1u) {
				std::copy(std::begin(cubic), std::end(cubic), out);
				continue;
			}

			const Double3 p[4] = { ToDouble3(cubic[0]), ToDouble3(cubic[1]), ToDouble3(cubic[2]), ToDouble3(cubic[3]) };
			for (uint32_t m = 0; m < count; ++m, out += 4) {
				const double a = double(m) / double(count);
				const double b = double(m + 1u) / double(count);
				out[0] = ToFloat3(Blossom(p, a, a, a));
				out[1] = ToFloat3(Blossom(p, a, a, b));
				out[2] = ToFloat3(Blossom(p, a, b, b));
				out[3] = ToFloat3(Blossom(p, b, b, b));
			}
		}
		piece_control_points->Upload(std::span<const XMFLOAT3>{ control_point_scratch }, context);
	}

	if (parts & DirtyColors) {
		color_scratch.resize(piece_total);
		for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
			const BezierData& bez = curves[curveIndex];
			const uint32_t c0 = PackFloat3ToR8G8B8A8(bez.C0), c1 = PackFloat3ToR8G8B8A8(bez.C1);
			const bool by_height = bez.min_height < bez.max_height;

			const uint32_t first = piece_first[curveIndex];
			const uint32_t count = piece_first[curveIndex + 1u] - first;
			for (uint32_t m = 0; m < count; ++m) {
				// Blend by t: the piece's range of the parent's t, reversed so it still reads as
				// "min >= max". Pieces 0 and count - 1 get exactly 0 and 1 at their outer ends.
				const float tA = float(double(m) / double(count));
				const float tB = float(double(m + 1u) / double(count));
				color_scratch[first + m] = by_height
					? UploadPieceColor{ c0, c1, bez.min_height, bez.max_height }
					: UploadPieceColor{ c0, c1, tB, tA };
			}
		}
		piece_colors->Upload(std::span<const UploadPieceColor>{ color_scratch }, context);
	}

	if (parts & DirtyStyles)
		UploadStyles(context);

	// Width, caps and join are styles too, so changing only those still recounts; harmless, not free.
	if (parts & (DirtyPositions | DirtyStyles))
		UpdatePatternBound();
	if (parts & (DirtyPositions | DirtyStyles | DirtyLayout))
		need_recount = true;

	// The chain each piece belongs to, as a piece range. A chain starts at the first piece of an
	// IsChainStart() curve and ends at the piece before the next one.
	if (parts & DirtyLayout) {
		chain_scratch.resize(piece_total);
		size_t chain_first_curve = 0;
		for (size_t curveIndex = 1; curveIndex <= curves.size(); ++curveIndex) {
			if (curveIndex == curves.size() || IsChainStart(curveIndex)) {
				const UploadChainPieces chain{ piece_first[chain_first_curve], piece_first[curveIndex] - 1u };
				for (uint32_t piece = chain.first_piece; piece <= chain.last_piece; ++piece)
					chain_scratch[piece] = chain;
				chain_first_curve = curveIndex;
			}
		}
		curve_chains->Upload(std::span<const UploadChainPieces>{ chain_scratch }, context);
	}
}

void BezierRenderer::RunPointPass(GraphicsDeviceContext* context) {
	ClearComputeBindings(context);

	// One thread per sample. The piece and t come from the sample index alone, so the control points
	// are the only thing it loads (and a piece's last sample, the joint duplicate, loads nothing).
	viewport_data->Bind(ShaderStage::Compute, 0, context);        // b0
	piece_control_points->Bind(ShaderStage::Compute, 0, context); // t0: P0..P3 per piece
	world_distances->BindUnordered(0, context);                   // u0
	screen_distances->BindUnordered(1, context);                  // u1

	profiler.begin_gpu("calc");
	calc_points->Run({ (total_points + 256u - 1u) / 256u, 1u, 1u }, context);
	profiler.end_gpu("calc");

	ClearComputeBindings(context);

	// Two independent exclusive scans over the same begin-bit flags: [i] becomes the arc at sample i.
	// `curve_begins` is read-only to the top-level pass, so the two share it safely; they need
	// separate instances only for the block-sum and carry scratch.
	profiler.begin_gpu("scan");
	world_scan.Scan(*world_distances, *curve_begins, total_points, context);
	screen_scan.Scan(*screen_distances, *curve_begins, total_points, context);
	profiler.end_gpu("scan");

	ClearComputeBindings(context);
}

// Sized from the CPU-side upper bound, so this runs before any compute pass and never waits on one.
// Rounded up by next_pow2 on top of a bound that already over-counts, and shrunk only at a quarter,
// so a scene edit usually costs no reallocation at all.
void BezierRenderer::AllocatePatternBuffer(const GraphicsDevice& device) {
	// Grow when the bound no longer fits, shrink once it has fallen to a quarter of the allocation
	// (removed curves, a larger spacing) - see FitCapacity() in bezier_common.
	const uint32_t required = FitCapacity(patterns_allocated, pattern_upper_bound);
	if (patterns && patterns_allocated == required) return;

	patterns.reset(new RWStructuredBuffer(device, TypedCapacityOrImmutableData<float>(required)));
	patterns_allocated = required;
}

// Count, then scan. No atomic anywhere in here, so curve i's slice of the pattern array is a
// function of the curve data alone - reproducible frame to frame, run to run, and across GPUs.
//
// There used to be a third dispatch after the scan - curve_pattern_resolve - to fold the offsets
// back into a uint2's .x and to publish the grand total. Neither survives: the counts are read as
// the gap between neighbouring offsets, and ParalellScan appends the total itself.
void BezierRenderer::CountPatternCenters(GraphicsDeviceContext* context) {
	if (piece_total == 0u || !pattern_offsets) return;

	ClearComputeBindings(context);

	// 1. Per piece, how many centres it has. Each thread writes only its own element.
	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	// World only - the count depends on world arc length alone, so screen_distances is not bound.
	world_distances->BindOrdered(ShaderStage::Compute, 0, context);   // t0: arc at [64k] and [64k + 63]
	curve_styles->Bind(ShaderStage::Compute, 1, context);             // t1
	pattern_offsets->BindUnordered(0, context);                       // u0

	pattern_ini->Run({ (piece_total + 256u - 1u) / 256u, 1u, 1u }, context);

	ClearComputeBindings(context);

	// 2. Exclusive prefix sum over those counts, in place: pattern_offsets[i] becomes piece i's base
	// index into the flat pattern array, with the grand total appended at [piece_total].
	offset_scan.Scan(*pattern_offsets, piece_total, context, true);

	ClearComputeBindings(context);
}

void BezierRenderer::RunPatternPass(GraphicsDeviceContext* context) {
	// Gated on the bound rather than on a count, because the count is deliberately a few frames old.
	// A bound of 0 means no curve in the scene has a positive spacing, which is current and exact.
	if (pattern_upper_bound == 0 || !patterns) return;

	ClearComputeBindings(context);

	viewport_data->Bind(ShaderStage::Compute, 0, context);            // b0
	world_distances->BindOrdered(ShaderStage::Compute, 0, context);   // t0: binary-search key
	screen_distances->BindOrdered(ShaderStage::Compute, 1, context);  // t1: what the hit interpolates
	curve_styles->Bind(ShaderStage::Compute, 2, context);             // t2
	pattern_offsets->BindOrdered(ShaderStage::Compute, 3, context);   // t3: base index per piece, total appended
	patterns->BindUnordered(0, context);                              // u0

	// 8 threads per piece on x, 8 pieces per group on y.
	pattern_calc->Run({ 1u, (piece_total + 8u - 1u) / 8u, 1u }, context);

	ClearComputeBindings(context);
}

void BezierRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	// need_recount is raised inside UploadCurves, only for the parts the count depends on.
	UpdateBuffers(device, context);

	if (total_points == 0 || !world_distances || !piece_control_points) {
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

	// The slots keep their old numbers; t1 (begin bits), t4 (index map) and t7 (sample ranges) are
	// simply not used any more - the sample index says which piece and which t.
	world_distances->BindOrdered(ShaderStage::Vertex, 0, context);     // t0: world arc per sample
	screen_distances->BindOrdered(ShaderStage::Vertex, 2, context);    // t2: screen arc per sample, px
	piece_control_points->Bind(ShaderStage::Vertex, 3, context);       // t3: P0..P3 per piece
	piece_colors->Bind(ShaderStage::Vertex, 5, context);               // t5: colours + height band / t range
	curve_styles->Bind(ShaderStage::Vertex, 6, context);               // t6: width_capcapjoin / spacing / dash length
	pattern_offsets->BindOrdered(ShaderStage::Vertex, 8, context);     // t8: pattern base per piece
	curve_chains->Bind(ShaderStage::Vertex, 9, context);               // t9: first/last piece of the chain

	// t1: screen arc length per pattern center. Its clamp bound used to be read here too, from
	// pattern_offsets[TotalCurveCount] - the SCENE's centre total, which let a curve read the next
	// chain's centres. The bound is the chain's slot range now, and it arrives as an interpolant
	// (PatternSlots) from curve_vs, so pattern_offsets is no longer bound to the pixel stage.
	if (patterns) patterns->BindOrdered(ShaderStage::Pixel, 1, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context); // b1
	viewport_data->Bind(ShaderStage::Pixel, 1, context);  // b1

	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	// 63 instances per piece, every one of them a real segment: curve_vs maps instance s to sample
	// s + s / 63, which skips each piece's joint duplicate and every bridge between chains.
	context->get()->DrawInstanced(5, piece_total * PieceSegments, 0, 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
