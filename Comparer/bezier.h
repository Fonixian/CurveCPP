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
// The pattern is CONTINUOUS ACROSS CURVES. bezier_common marks one segment begin for the whole
// scene, so the prefix sum runs over the chain and the centres sit on one grid of world distances
// n * spacing shared by every curve in it. pattern_ini gives each curve the window of that grid
// lying inside its own arc span rather than restarting at n = 0, pattern_calc places exactly those
// n, and curve_vs hands the pixel shader the bias that turns a global n back into a slot in the
// flat array (plus one slot of slack at each end, so a dash centred just past a joint still draws
// on the near side of it). Two curves joined end to end therefore continue one dash sequence
// instead of each starting a fresh dash at their shared endpoint. This mirrors what bezier_dots
// does - dot_ini.hlsl / dot_calc.hlsl take the same window - except that dots need no slack,
// because each dot is its own quad and owes nothing to the geometry it sits on.
//
// If every curve in the scene is solid, use BezierSolidRenderer instead - it skips all of it.
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
	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	uint32_t PatternCapacity() const override { return patterns_allocated; }

protected:
	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocatePatternBuffer(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void CountPatternCenters(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// Set whenever the curve data changed, so the centre count is recomputed once rather than every
	// frame. The count depends on world arc length only, which the camera does not move.
	bool need_recount = false;

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
