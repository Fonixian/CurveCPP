#pragma once
#include "bezier_common.h"
#include "SegmentedScan.h"

// The patterned curve renderer. The pattern is described per curve by two numbers rather than an
// enum: BezierData::spacing (world arc length between dash centres) and BezierData::dash_length
// (length of one dash, in pixels). A dot is a zero-length dash with round caps; a solid stroke is
// spacing <= 0, which yields no pattern centres at all and leaves the body untouched.
//
// Everything here exists because a dash is spaced along the curve by WORLD arc length but drawn at a
// fixed size in PIXELS, so the pixel shader has to be told where each pattern centre landed in
// screen arc length. Getting there costs four things the solid renderer does without:
//
//   distances        a per-point float2 of (world, screen) segment length
//   scan             a segmented prefix sum turning both channels into per-curve running totals
//   pattern_ini      counts the centres per curve and hands back a total, which the CPU reads back
//   pattern_calc     binary-searches the world sum per centre and samples the screen sum there
//
// If every curve in the scene is solid, use BezierSolidRenderer instead - it skips all of it.

class BezierRenderer : public BezierRendererBase {
public:
	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void CountPatternCenters(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// Set whenever the curve data changed, so the centre count is recomputed once rather than every
	// frame. The count depends on world arc length only, which the camera does not move.
	bool need_recount = false;

	uint32_t pattern_total = 0;
	uint32_t patterns_allocated = 0;

	// --- per-frame compute results -------------------------------------------------
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;        // (world, screen) arc length, prefix-summed per curve
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_counter;  // Single uint, atomically summed pattern count
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_ranges;   // Per curve: uint2(first pattern, pattern count)
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> patterns;         // One float per pattern: screen arc length of the center

	Axodox::Graphics::ComputeShader* pattern_ini;
	Axodox::Graphics::ComputeShader* pattern_calc;
	SegmentedScan scan;
};
