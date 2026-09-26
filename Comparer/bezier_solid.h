#pragma once
#include "bezier_common.h"

// The solid renderer's curve record - one per curve, 32 bytes, mirrored by BezierCurveData in
// shaders/bezier_solid/solid_common.hlsli. Style lives here too, so there is no separate style buffer.
//
// The curve itself is NOT here and is not raised to cubic: its control points go into their own
// float3 buffer exactly as given - 2, 3 or 4 of them for a linear, quadratic or cubic curve - and
// this record points at them with control_first plus the count in the top byte of the last word.
// solid_vert evaluates the Bernstein form of whatever degree it finds.
//
//   control_first   first control point of this curve in the ControlPoints buffer
//   first_index     first SAMPLE index of this curve; t of sample i = (i - first) / (resolution - 1).
//                   A merged curve's first sample is the previous curve's last one (the shared joint,
//                   see BezierData::merge_with_previous).
//   resolution      sample count, both ends included
//   count_caps_join control point count << 24 | cap_front << 16 | cap_back << 8 | join. The low 24
//                   bits are the same CapCapJoin every other renderer packs, so FrontCap / BackCap /
//                   Join in the shaders read it unchanged; ControlCount reads the top byte.
//   color_begin/end R8G8B8A8, as before
//   height_range    min_height in the low 16 bits, max_height in the high 16, both IEEE half floats
//                   (f16tof32 in HLSL). Half rather than a fixed 0..1000 range because heights are
//                   world Y and go negative (the example's band is -5..5).
//   width           stroke width in pixels
struct UploadSolidCurveData {
	uint32_t control_first;
	uint32_t first_index;
	uint32_t resolution;
	uint32_t count_caps_join;
	uint32_t color_begin;
	uint32_t color_end;
	uint32_t height_range;
	float    width;
};
static_assert(sizeof(UploadSolidCurveData) == 32, "UploadSolidCurveData must match BezierCurveData in solid_common.hlsli");

class BezierSolidRenderer : public BezierRendererBase {
public:
	explicit BezierSolidRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	// solid_vert evaluates every sample it needs from the curve definitions itself, so there is no
	// point pass and no per-sample buffer for it to fill.
	bool NeedsCalculatedPoints() const override { return false; }

	void AllocateCurveData(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadCurveData(Axodox::Graphics::GraphicsDeviceContext* context) override;

private:
	// float3 per control point, sized for the worst case of 4 per curve. That way it only has to be
	// reallocated together with bezier_data: a curve changing degree through control_points() is a
	// plain upload (touch()), not a layout change, so a buffer sized to the exact count could be
	// outgrown without any resize ever being requested. Only the live prefix is uploaded.
	std::unique_ptr<Axodox::Graphics::StructuredBuffer> control_points;
};
