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
//   world_distances / screen_distances   per-point float buffers, segment length in world / screen
//   world_scan / screen_scan             a segmented prefix sum per buffer, turning each into
//                                        per-curve running totals
//   pattern_ini                          counts the centres per curve
//   offset_scan                          an exclusive prefix sum turning those counts into each
//                                        curve's slice of the pattern array
//   pattern_calc                         binary-searches the world sum per centre and samples the
//                                        screen sum there
//
// The pattern is CONTINUOUS ACROSS THE CURVES OF A MERGED CHAIN, and only across those.
// bezier_common marks one segment begin per CHAIN, so each prefix sum runs over one chain, restarts
// at 0 at the next, and the centres of a chain sit on one grid of world distances n * spacing shared
// by every curve in it. pattern_ini gives each curve the window of that grid lying inside its own
// arc span, pattern_calc places exactly those n, and curve_vs hands the pixel shader the bias that
// turns a grid n back into a slot in the flat array. Two curves merged end to end therefore continue
// one dash sequence instead of each starting a fresh dash at their shared endpoint. bezier_dots takes
// the same window.
//
// The flat array is one sorted run PER CHAIN, laid end to end in curve order - NOT one sorted run for
// the scene: the screen arc of the next chain's first centre is 0 again. A pixel may look at any
// centre of its own chain (a dash centred just past a joint still draws on the near side of it) and
// at nothing outside it, so curve_vs hands the pixel shader the chain's slot range,
// [PatternOffsets[first curve], PatternOffsets[last curve + 1]), built from curve_chains below.
//
// Curve data is the base class's split upload: control points, colours and sample ranges in three
// buffers, plus one PatternStyle (width_capcapjoin, spacing, dash_length) per curve re-sent only
// when a style setter fired. curve_vs builds the strip the way solid_vert does - B, C and both
// neighbours evaluated straight from the control points, a neighbour across a merged joint taken
// from the adjacent curve - so there is no per-sample position buffer; the point pass keeps only the
// two arc lengths. The terminus test is per segment (Neighbors, from the begin bits).
//
// Nothing in a frame waits on a compute result: the pattern buffer is sized from
// BezierRendererBase::PatternBound(), a CPU-side upper bound, and the exact centre total stays on the
// GPU, where curve_vs reads it.
//
// World and screen arc length are separate float buffers with their own SegmentedScan each, so every
// consumer loads only the channel it reads: pattern_ini and the binary search in pattern_calc touch
// world only, the interpolation at the end of pattern_calc and curve_vs's ScreenArcBegin/End touch
// screen. An instance owns its block-sum/carry scratch, hence two instances.
//
// The slice each curve gets is a prefix sum (offset[i] is the sum of counts[0..i-1] and nothing
// else), so the layout is reproducible from frame to frame and machine to machine.

class BezierRenderer : public BezierRendererBase {
public:
	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	uint32_t PatternCapacity() const override { return patterns_allocated; }

protected:
	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
	// The base upload, plus what the pattern passes need on top of it: the CPU-side pattern bound and
	// need_recount - raised only when something the centre count depends on changed, so a
	// colour-only change does not recount.
	void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocatePatternBuffer(const Axodox::Graphics::GraphicsDevice& device);
	void CountPatternCenters(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// Set whenever positions, sample layout or styles changed, so the centre count is recomputed once
	// rather than every frame. The count depends on world arc length and spacing only, which the
	// camera does not move. Colours never raise it. See UploadCurves.
	bool need_recount = false;

	// Matches PatternStyle in curve_common.hlsli: PackWidthCapCapJoin, then spacing and dash length.
	struct UploadPatternStyle {
		uint32_t width_capcapjoin;
		float    spacing;
		float    dash_length;
	};
	static_assert(sizeof(UploadPatternStyle) == 12, "UploadPatternStyle must match PatternStyle in the shaders");

	std::vector<UploadPatternStyle> style_scratch;

	// Matches StructuredBuffer<uint2> CurveChains in curve_vs.hlsl: the first and last CURVE index of
	// the merged chain a curve belongs to (both itself for an unmerged curve). Changes only with the
	// layout (Add / remove / Merged / Resolution), so it is re-sent on DirtyLayout alone.
	struct UploadChainCurves {
		uint32_t first_curve;
		uint32_t last_curve;
	};
	static_assert(sizeof(UploadChainCurves) == 8, "UploadChainCurves must match CurveChains in curve_vs.hlsl");

	std::vector<UploadChainCurves> chain_scratch;
	// One UploadChainCurves per curve. curve_vs turns it into the chain's slot range in the pattern
	// array, which is all the pixel shader is allowed to read.
	std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_chains;

	uint32_t patterns_allocated = 0;

	// --- per-frame compute results -------------------------------------------------
	// One float per point each, written as a segment length by curve_calc_points and turned into a
	// chain-cumulative running total by the matching scan below. Kept apart rather than interleaved
	// into one float2 so each consumer loads only the channel it reads.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> world_distances;   // world arc length
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> screen_distances;  // screen arc length, px
	// Per curve: written as the centre count by pattern_ini, then scanned IN PLACE into that curve's
	// base offset, with the grand total appended one slot past the last curve. That appended slot is
	// why there is no second buffer of counts and no resolve pass: curve i's count is the gap to
	// offset i + 1, valid for the last curve too, and the total is a plain load at [curve count].
	// Allocated one element longer than the curve count for it.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_offsets;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> patterns;        // One float per pattern: screen arc length of the center

	Axodox::Graphics::ComputeShader* pattern_ini;
	Axodox::Graphics::ComputeShader* pattern_calc;
	// One instance per distance buffer: an instance owns the block-sum and carry scratch a scan runs
	// through, so the two channels cannot share one.
	SegmentedScan world_scan;
	SegmentedScan screen_scan;
	// Over curve counts, not points - at most one element per curve, so its own buffers are tiny.
	ParalellScan offset_scan;
};
