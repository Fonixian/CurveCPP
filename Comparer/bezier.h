#pragma once
#include "bezier_common.h"
#include "SegmentedScan.h"
#include "ParalellScan.h"

// The patterned curve renderer. The pattern is described per curve by two numbers rather than an
// enum: BezierData::spacing (world arc length between dash centres) and BezierData::dash_length
// (length of one dash, in pixels). A dot is a zero-length dash with round caps. There is no solid
// mode: spacing <= 0 draws nothing - solid strokes belong in BezierSolidRenderer.
//
// Everything here exists because a dash is spaced along the curve by WORLD arc length but drawn at a
// fixed size in PIXELS, so the pixel shader has to be told where each pattern centre landed in
// screen arc length. Getting there costs four things the solid renderer does without:
//
//   distances        two per-point float buffers, world and screen segment length
//   scan             a segmented prefix sum per buffer, turning each into per-curve running totals
//   pattern_ini      counts the centres per curve
//   offset_scan      an exclusive prefix sum turning those counts into each curve's slice
//   pattern_calc     binary-searches the world sum per centre and samples the screen sum there
//
// The pattern is CONTINUOUS ACROSS THE CURVES OF A MERGED CHAIN, and only across those.
// bezier_common marks one segment begin per CHAIN, so each prefix sum runs over one chain, restarts
// at 0 at the next, and the centres of a chain sit on one grid of world distances n * spacing shared
// by every curve in it. pattern_ini gives each curve the window of that grid lying inside its own
// arc span rather than restarting at n = 0, pattern_calc places exactly those n, and curve_vs hands
// the pixel shader the bias that turns a grid n back into a slot in the flat array. Two curves
// merged end to end therefore continue one dash sequence instead of each starting a fresh dash at
// their shared endpoint. This mirrors what bezier_dots does - dot_ini.hlsl / dot_calc.hlsl take the
// same window.
//
// The flat array is therefore one sorted run PER CHAIN, laid end to end in curve order - NOT one
// sorted run for the scene: the screen arc of the next chain's first centre is 0 again. A pixel may
// look at any centre of its own chain (a dash centred just past a joint still draws on the near side
// of it) and at nothing outside it, so curve_vs hands the pixel shader the chain's slot range,
// [PatternOffsets[first curve], PatternOffsets[last curve + 1]), built from curve_chains below. Before
// that range existed the pixel shader clamped to the whole SCENE's slots, and a curve's last dash
// measured its neighbour gap against the next, unrelated curve's first centre - so adding a curve
// changed how the curve before it was drawn.
//
// If every curve in the scene is solid, use BezierSolidRenderer instead - it skips all of it.
//
// FIXED-SIZE PIECES. The GPU never sees a BezierData. Every curve is cut, on the CPU, into
// ceil(resolution / 64) sub-curves of exactly PieceSamples = 64 sample points (63 segments) each - a
// 192-point curve becomes three pieces over t in [0, 1/3], [1/3, 2/3], [2/3, 1] - and those pieces are
// what every buffer below is indexed by. Because every piece has the same sample count, a sample
// index says everything about where it lives:
//
//     piece = sample >> 6        local = sample & 63        t = local / 63   (local 63 is exactly 1)
//
// so no shader needs a per-sample curve map, a per-curve sample range or a divide by (last - first):
// BezierIndexMap and CurveIndices are gone from this renderer, and the control points of a sample can
// be fetched the moment its index is known. The resolution a curve asks for is rounded UP to a whole
// number of pieces, so a short curve at resolution 2 costs a full 64-sample piece.
//
// Pieces do NOT share their joint sample: a piece's last sample (t = 1) and the next piece's first
// (t = 0) are the same point, stored twice. That keeps every piece a 64-sample block. The duplicate
// costs one zero-length "segment" per piece: curve_calc_points writes 0 for a piece's last sample,
// so the arc lengths simply carry across the joint (x + 0 == x exactly), and the draw skips it - it
// draws 63 instances per piece and maps instance s to sample s + s / 63.
//
// The pieces of one curve are a merged chain of their own (the first one merged into the previous
// curve iff the curve is), so the pattern grid, the caps and the joins run across the cuts exactly as
// they run across a merged joint. The cuts are taken by blossoming the cubic in double precision, so
// the joint point is computed by the same arithmetic on both sides and is bit-identical, and the
// first and last pieces keep the curve's own P0 and P3.
//
// Colour: a curve blended by t (no height band) would restart its blend in every piece, so a piece
// carries its parent-t range [tA, tB] instead, in the two height floats it does not otherwise use -
// stored as (tB, tA), so that "min >= max" still reads as "blend by t". The vertex shader blends by
// lerp(tA, tB, t), which is the parent's t exactly; the endpoint colours stay the parent's C0 / C1,
// so nothing is re-quantised to 8 bits at a cut. A curve with a height band does not care about t
// and keeps its heights.
//
// Per piece there is one PatternStyle (width_capcapjoin, spacing, dash_length), a colour (see
// above), the four control points and the chain's first / last piece, each re-sent only when its
// part changed (CurveDirtyBits). Positions re-split every curve; layout (Add / Remove / Resolution /
// Merged) re-cuts the piece table.
//
// The centre count used to come back from pattern_ini through a blocking Download(), which drains the
// whole GPU queue in the middle of the frame just to size one buffer. It does not any more: the
// buffer is sized from BezierRendererBase::PatternBound(), a CPU-side upper bound that needs no GPU
// result at all. Nothing in a frame ever waits on a compute result now, and nothing mirrors the
// exact count back either - the scan's appended total stays on the GPU, where curve_vs reads it.
//
// The two arc-length channels used to live in ONE buffer of float2 and ride through a single
// float2-typed SegmentedScan. They are two float buffers scanned by two SegmentedScan instances now.
// The same number of floats is written and summed either way, so this is not about bandwidth; it is
// that nothing outside this renderer ever wanted both channels at once. The scan is a scalar merge
// again, bezier_dots stops carrying a zero-filled second channel through it, and every consumer here
// reads exactly the channel it needs: pattern_ini and the binary search in pattern_calc touch world
// only, the interpolation at the end of pattern_calc and curve_vs's ScreenArcBegin/End touch screen.
// The cost is a second set of the scan's block-sum/carry scratch (~5 MB at maxElementCount), and two
// dispatch chains where there was one.
//
// The slice each curve gets used to come from an InterlockedAdd on a shared counter, so it depended
// on the order the thread groups happened to retire: the same scene could lay its centres out
// differently from one frame, or one machine, to the next. It is a prefix sum now - offset[i] is the
// sum of counts[0..i-1] and nothing else - so the layout is reproducible. The cost is one extra uint
// per curve (pattern_offsets) plus ParalellScan's block-sum buffers, and two dispatches where there
// was one; an atomic needs no room to work in, a scan does.

class BezierRenderer : public BezierRendererBase {
public:
	// Sample points per GPU piece, as a shift. Must match PieceSampleShift in curve_common.hlsli.
	static constexpr uint32_t PieceSampleShift = 6u;
	static constexpr uint32_t PieceSamples = 1u << PieceSampleShift;  // 64
	static constexpr uint32_t PieceSegments = PieceSamples - 1u;      // 63 - the last sample is the joint duplicate

	// How many pieces a curve of `resolution` sample points is cut into: ceil(resolution / 64), never
	// fewer than one.
	static uint32_t PieceCount(unsigned resolution);

	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	uint32_t PatternCapacity() const override { return patterns_allocated; }

	// GPU pieces in the scene - what the shaders see as TotalCurveCount.
	uint32_t PieceTotal() const { return piece_total; }

protected:
	bool NeedsCalculatedPoints() const override { return false; }
	bool NeedsBezierData() const override { return false; }
	uint32_t GpuCurveCount() const override { return piece_total; }

	// The piece layout: cuts every curve into PieceCount() pieces, sizes every buffer by pieces and
	// samples, and sets one begin bit per chain at the first sample of its first piece.
	void LayoutBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context) override;

	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
	// Per-piece uploads, each only when its part is dirty, plus what the pattern passes need on top:
	// the CPU-side pattern bound and need_recount - raised only when something the centre count
	// depends on changed, so a colour-only change does not recount.
	void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocatePatternBuffer(const Axodox::Graphics::GraphicsDevice& device);
	void CountPatternCenters(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// Set whenever positions, layout or styles changed, so the centre count is recomputed once
	// rather than every frame. The count depends on world arc length and spacing only, which the
	// camera does not move. Colours never raise it. See UploadCurves.
	bool need_recount = false;

	// PatternBound() over the live curves, from the cubic control polygons and spacings. Taken per
	// CURVE, not per piece: the piece counts of one curve telescope to the curve's own window of the
	// chain grid, so the per-curve bound already covers all of its pieces.
	void UpdatePatternBound();

	// piece_first[c] is the first piece of curve c; piece_first[curves.size()] == piece_total. Rebuilt
	// by LayoutBuffers, so it is valid in every upload.
	std::vector<uint32_t> piece_first;
	uint32_t piece_total = 0;

	// Matches PatternStyle in curve_common.hlsli: width << 24 | cap_front << 16 | cap_back << 8 |
	// join, then spacing and dash length. The width takes the top byte, so it is rounded to a WHOLE
	// pixel and clamped to [0, 255] like the solid renderer's.
	struct UploadPatternStyle {
		uint32_t width_capcapjoin;
		float    spacing;
		float    dash_length;
	};
	static_assert(sizeof(UploadPatternStyle) == 12, "UploadPatternStyle must match PatternStyle in the shaders");

	// Matches the uint4 CurveColors in curve_vs.hlsl. z / w are the height band when min < max,
	// otherwise the piece's parent-t range stored as (tB, tA) - see the note above.
	struct UploadPieceColor {
		uint32_t color_begin;
		uint32_t color_end;
		float    z;
		float    w;
	};
	static_assert(sizeof(UploadPieceColor) == 16, "UploadPieceColor must match CurveColors in curve_vs.hlsl");

	// Matches StructuredBuffer<uint2> CurveChains: the first and last PIECE of the chain a piece
	// belongs to. Changes only with the layout, so it is re-sent on DirtyLayout alone.
	struct UploadChainPieces {
		uint32_t first_piece;
		uint32_t last_piece;
	};
	static_assert(sizeof(UploadChainPieces) == 8, "UploadChainPieces must match CurveChains in the shaders");

	// Staging vectors, kept between uploads so a per-frame re-pose does not allocate.
	std::vector<DirectX::XMFLOAT3>  control_point_scratch;
	std::vector<UploadPieceColor>   color_scratch;
	std::vector<UploadPatternStyle> style_scratch;
	std::vector<UploadChainPieces>  chain_scratch;
	std::vector<uint32_t>           begin_scratch;

	// Per piece: P0..P3 at piece * 4 (48 B), the colour (16 B), the chain range (8 B). The style
	// buffer is the base's curve_styles, also per piece.
	std::unique_ptr<Axodox::Graphics::StructuredBuffer> piece_control_points;
	std::unique_ptr<Axodox::Graphics::StructuredBuffer> piece_colors;
	std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_chains;

	uint32_t patterns_allocated = 0;

	// --- per-frame compute results -------------------------------------------------
	// One float per SAMPLE each, written by curve_calc_points as the chord to the next sample (0 for a
	// piece's last sample, the joint duplicate) and turned into an exclusive chain-cumulative running
	// total by the matching scan below: [i] is the arc at sample i, so a piece spans [64k] .. [64k + 63].
	// Kept apart rather than interleaved so each consumer loads only the channel it reads.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> world_distances;   // world arc length
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> screen_distances;  // screen arc length, px
	// Per piece: written as the centre count by pattern_ini, then scanned IN PLACE into that piece's
	// base offset, with the grand total appended one slot past the last piece - so a piece's count is
	// the gap to offset i + 1 and the total is a plain load at [piece count]. Allocated one longer.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_offsets;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> patterns;        // One float per pattern: screen arc length of the center

	Axodox::Graphics::ComputeShader* pattern_ini;
	Axodox::Graphics::ComputeShader* pattern_calc;
	// One instance per distance buffer: an instance owns the block-sum and carry scratch a scan runs
	// through, so the two channels cannot share one.
	SegmentedScan world_scan;
	SegmentedScan screen_scan;
	// Over piece counts, not samples - at most one element per piece, so its own buffers are tiny.
	ParalellScan offset_scan;
};
