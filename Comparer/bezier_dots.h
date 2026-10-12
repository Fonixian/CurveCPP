#pragma once
#include "bezier_common.h"
#include "gpu_buffers.h"
#include "SegmentedScan.h"
#include "ParalellScan.h"

// The dot/point renderer. Draws ONLY dot patterns: spacing <= 0 means no dots at all for that curve -
// there is no "solid" fallback the way spacing <= 0 means solid in BezierRenderer, because this
// renderer never draws a stroke body. dash_length is never read.
//
// BezierRenderer places a dash/dot by finding the SCREEN arc length of its centre and letting the
// pixel shader carve a cap shape out of the line-strip's already-rasterised quad - that is what needs
// the segmented scan of world AND screen arc length. This renderer instead finds each dot's WORLD
// position and tangent DIRECTION directly (a segmented scan + binary search over WORLD arc length
// only - screen arc length is never computed) and draws its own small instanced quad there:
// screen-aligned, sized by width in pixels, oriented by the projected tangent. That is the DX11
// stand-in for GL_POINTS with gl_PointSize. There is no shared strip geometry, so there is nothing to
// kink at a seam - every dot is a self-contained draw.
//
// A dot's shape is cap_front/cap_back exactly like BezierRenderer's dash_length == 0 case: the half of
// the quad facing back toward the curve's start (P0) is shaped by cap_front, the half facing forward
// toward its end (P3) by cap_back, split along the projected tangent (dot_ps.hlsl). The FRONT/BACK
// choice is decided from the tangent's sign, which is exact - unlike curve_ps.hlsl's version of this
// same split, which compares a world arc length against a pixel offset. Not replicating that is
// deliberate.
//
// The dot count IS the instance count of the draw, so it must be the current frame's: it never comes
// back to the CPU, dot_args writes it straight into a DrawInstancedIndirect argument buffer on the
// GPU. The dot buffer itself is sized from BezierRendererBase::PatternBound().
//
// Curve data is the base class's split upload (control points, colours and sample ranges), plus one
// DotStyle (width_capcap + spacing) per curve re-sent only when a style setter fired. None of the
// solid renderer's merged-joint handling is needed here: a dot is a point on one curve, so the draw
// binds no curve_begins and no index map. The point pass still reads the
// index map, which is what hands a joint sample to the curve leaving it.

class BezierDotRenderer : public BezierRendererBase {
public:
	explicit BezierDotRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	uint32_t PatternCapacity() const override { return dots_allocated; }

protected:
	// dot_vert evaluates the curve at each dot's t straight from the control points, so nothing in
	// this renderer stores a sample position or colour. See dot_calc_points.hlsl.

	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
	// The base upload, plus what the dot passes need on top of it: the CPU-side pattern bound and
	// need_recount - raised only when something the dot count depends on changed, so a colour-only
	// change does not recount.
	void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocateDotBuffer(const Axodox::Graphics::GraphicsDevice& device);
	void CountDots(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunDotPlacementPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// Set whenever positions, sample layout or styles changed, so the dot count is recomputed once
	// rather than every frame - the count depends on world arc length and spacing only, which the
	// camera does not move. Colours never raise it. See UploadCurves.
	bool need_recount = false;

	// Matches DotStyle in dot_common.hlsli: PackWidthCapCapJoin with the join byte 0, then the spacing.
	struct UploadDotStyle {
		uint32_t width_capcap;
		float    spacing;
	};
	static_assert(sizeof(UploadDotStyle) == 8, "UploadDotStyle must match DotStyle in the shaders");

	std::vector<UploadDotStyle> style_scratch;

	uint32_t dots_allocated = 0;

	// --- per-frame compute results -------------------------------------------------
	// World arc length, one float per point, prefix-summed per curve.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;

	// Per curve: written as the dot count by dot_ini, then scanned IN PLACE into that curve's base
	// index within `dots`, with the grand total appended one slot past the last curve. That appended
	// slot is why there is no second buffer of counts and no resolve pass: curve i's count is the gap
	// to index i + 1, valid for the last curve too, and dot_args reads the instance count straight
	// out of [curve count]. Allocated one element longer than the curve count for it.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dot_indices;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dots;        // per dot: t along its curve + curve index

	IndirectDrawArgs draw_args;

	Axodox::Graphics::ComputeShader* dot_ini;
	Axodox::Graphics::ComputeShader* dot_calc;
	Axodox::Graphics::ComputeShader* dot_args;
	SegmentedScan<float> scan;
	// Over curve counts, not points - at most one element per curve, so its own buffers are tiny.
	ParalellScan offset_scan;
};
