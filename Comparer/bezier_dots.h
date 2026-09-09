#pragma once
#include "bezier_common.h"
#include "SegmentedScan.h"

// The dot/point renderer. Draws ONLY dot patterns: spacing <= 0 means no dots at all for that curve -
// there is no "solid" fallback the way spacing <= 0 means solid in BezierRenderer, because this
// renderer never draws a stroke body. dash_length is never read.
//
// BezierRenderer places a dash/dot by finding the SCREEN arc length of its centre and letting the
// pixel shader carve a cap shape out of the line-strip's already-rasterised quad - that is what needs
// the segmented scan of (world, screen) arc length, the bisector arc shear, and the shrink-to-fit
// sizing documented in pattern_seam.md. This renderer instead finds each dot's WORLD position and
// tangent DIRECTION directly (still via a segmented scan + binary search over WORLD arc length only -
// screen arc length is never computed) and draws its own small instanced quad there: screen-aligned,
// sized by width in pixels, oriented by the projected tangent. That is the DX11 stand-in for
// GL_POINTS with gl_PointSize. There is no shared strip geometry, so there is nothing to kink at a
// seam - every dot is a self-contained draw.
//
// A dot's shape is cap_front/cap_back exactly like BezierRenderer's dash_length == 0 case: the half of
// the quad facing back toward the curve's start (P0) is shaped by cap_front, the half facing forward
// toward its end (P3) by cap_back, split along the projected tangent (dot_ps.hlsl). The FRONT/BACK
// choice is decided from the tangent's sign, which is exact - unlike curve_ps.hlsl's version of this
// same split, which compares a world arc length against a pixel offset (see the "Unrelated thing
// spotted" note in pattern_seam.md) and is a known bug there. Not replicating that bug is deliberate.

class BezierDotRenderer : public BezierRendererBase {
public:
	explicit BezierDotRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void CountDots(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void RunDotPlacementPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// Set whenever the curve data changed, so the dot count is recomputed once rather than every
	// frame - the count depends on world arc length only, which the camera does not move.
	bool need_recount = false;

	uint32_t dot_total = 0;
	uint32_t dots_allocated = 0;

	// --- per-frame compute results -------------------------------------------------
	// (world, 0) arc length, prefix-summed per curve. The second channel is unused - it exists only
	// because SegmentedScan's element type is fixed at float2 (shared, unmodified, with the patterned
	// renderer) - screen arc length is never needed here.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dot_counter; // single uint, atomically summed dot count
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dot_ranges;  // per curve: uint2(first dot, dot count)
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dots;        // per dot: bracketing sample pair + t between them

	Axodox::Graphics::ComputeShader* dot_ini;
	Axodox::Graphics::ComputeShader* dot_calc;
	SegmentedScan scan;
};
