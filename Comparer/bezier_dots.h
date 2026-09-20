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
// the segmented scan of world AND screen arc length, the bisector arc shear, and the shrink-to-fit
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
//
// This renderer has one CPU dependency on the dot count that the patterned one does not: the count IS
// the instance count of the draw. A count read back late cannot serve that - drawing more instances
// than the current frame's dot_calc wrote would resurrect dots from the previous scene - so the count
// never comes back to the CPU at all: dot_args writes it straight into a DrawInstancedIndirect
// argument buffer on the GPU. Nothing mirrors it back to the CPU either - the exact count is a
// diagnostic neither this renderer nor the patterned one pays for any more.

class BezierDotRenderer : public BezierRendererBase {
public:
	explicit BezierDotRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	uint32_t PatternCapacity() const override { return dots_allocated; }

protected:
	// dot_vert evaluates the two samples bracketing each dot straight from the control points, so
	// nothing in this renderer reads a stored sample position or colour. See dot_calc_points.hlsl.
	bool NeedsCalculatedPoints() const override { return false; }

	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocateDotBuffer(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void CountDots(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunDotPlacementPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// Set whenever the curve data changed, so the dot count is recomputed once rather than every
	// frame - the count depends on world arc length only, which the camera does not move.
	bool need_recount = false;

	uint32_t dots_allocated = 0;

	// --- per-frame compute results -------------------------------------------------
	// World arc length, one float per point, prefix-summed per curve. It used to be a float2 whose
	// second channel was always 0, because SegmentedScan's element type was fixed at float2 for the
	// patterned renderer's benefit; that renderer keeps its world and screen channels in separate
	// buffers now and the scan is scalar, so this renderer stops storing and summing a dead channel.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;

	// Per curve: written as the dot count by dot_ini, then scanned IN PLACE into that curve's base
	// index within `dots`, with the grand total appended one slot past the last curve. That appended
	// slot is why there is no second buffer of counts and no resolve pass: curve i's count is the gap
	// to index i + 1, valid for the last curve too, and dot_args reads the instance count straight
	// out of [curve count]. Allocated one element longer than the curve count for it.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dot_indices;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dots;        // per dot: bracketing sample pair + t between them

	IndirectDrawArgs draw_args;

	// dots_allocated, so dot_calc can clamp and dot_args can cap the instance count.
	PatternCapacityBuffer capacity_cb_data{};
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> dot_capacity;

	Axodox::Graphics::ComputeShader* dot_ini;
	Axodox::Graphics::ComputeShader* dot_calc;
	Axodox::Graphics::ComputeShader* dot_args;
	SegmentedScan scan;
	// Over curve counts, not points - at most one element per curve, so its own buffers are tiny.
	ParalellScan offset_scan;
};
