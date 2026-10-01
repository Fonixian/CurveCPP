#pragma once
#include "bezier_common.h"
#include "SegmentedScan.h"
#include "ParalellScan.h"

// The patterned curve renderer. The pattern is described per curve by two numbers rather than an
// enum: BezierData::spacing (world arc length between dash centres) and BezierData::dash_length
// (length of one dash, in pixels). A dot is a zero-length dash with round caps; a solid stroke is
// spacing <= 0, which yields no pattern centres at all and leaves the body untouched.
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
// Curve data is BezierSplitRendererBase's split upload, like the other three renderers: control
// points (K0..K3), colours and sample ranges in three buffers, each re-sent only when its part
// changes, plus one PatternStyle (width_capcapjoin, spacing, dash_length) per curve re-sent only when
// a style setter fired. curve_vs builds the strip the way solid_vert does - B, C and both neighbours
// evaluated straight from the control points, a neighbour across a merged joint taken from the
// adjacent curve - so there is no CalculatedPoints buffer; the point pass keeps only the two arc
// lengths. The chain range is gone from the per-curve data with the 80-byte struct: the terminus test
// is per segment (Neighbors, from the begin bits), as in the solid renderer.
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

class BezierRenderer : public BezierSplitRendererBase {
public:
	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	uint32_t PatternCapacity() const override { return patterns_allocated; }

protected:
	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
	// The base upload, plus what the pattern passes need on top of it: the CPU-side pattern bound
	// (the split base does not compute it) and need_recount - raised only when something the centre
	// count depends on changed, so a colour-only change no longer recounts.
	void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocatePatternBuffer(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void CountPatternCenters(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// Set whenever positions, sample layout or styles changed, so the centre count is recomputed once
	// rather than every frame. The count depends on world arc length and spacing only, which the
	// camera does not move. Colours never raise it. See UploadCurves.
	bool need_recount = false;

	// PatternBound() over the live curves, from the cubic control polygons and spacings.
	void UpdatePatternBound();

	// Matches PatternStyle in curve_common.hlsli: width << 24 | cap_front << 16 | cap_back << 8 |
	// join, then spacing and dash length. The low three bytes are the solid renderer's CapCapJoin; the
	// width takes the top byte, so it is rounded to a WHOLE pixel and clamped to [0, 255] like the
	// solid renderer's.
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
	// into one float2 so each consumer loads only the channel it reads; see the note above.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> world_distances;   // world arc length
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> screen_distances;  // screen arc length, px
	// Per curve: written as the centre count by pattern_ini, then scanned IN PLACE into that curve's
	// base offset, with the grand total appended one slot past the last curve. That appended slot is
	// why there is no second buffer of counts and no resolve pass: curve i's count is the gap to
	// offset i + 1, valid for the last curve too, and the total is a plain load at [curve count].
	// Allocated one element longer than the curve count for it.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_offsets;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> patterns;        // One float per pattern: screen arc length of the center

	// patterns_allocated, so pattern_calc can clamp rather than run off the end of the buffer.
	PatternCapacityBuffer capacity_cb_data{};
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> pattern_capacity;

	Axodox::Graphics::ComputeShader* pattern_ini;
	Axodox::Graphics::ComputeShader* pattern_calc;
	// One instance per distance buffer: an instance owns the block-sum and carry scratch a scan runs
	// through, so the two channels cannot share one.
	SegmentedScan world_scan;
	SegmentedScan screen_scan;
	// Over curve counts, not points - at most one element per curve, so its own buffers are tiny.
	ParalellScan offset_scan;
};
