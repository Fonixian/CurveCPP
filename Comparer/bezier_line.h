#pragma once
#include "bezier_common.h"

// Hardware line primitive only: a D3D11 LINELIST (the GL_LINES equivalent) with no geometry shader, so
// every curve is 1px wide whatever its `width`, uncapped, unjoined and unpatterned. It has no style
// buffer and no `bezier_data`: the per-curve data is split by how often it changes, into three buffers
// that are re-uploaded independently (see UploadCurves):
//
//   curve_control_points   the control points as given, NOT raised to cubic and NOT in monomial form:
//                          power + 1 float3 per curve (24 / 36 / 48 B)   when a curve is re-posed
//   curve_colors           LineColorData                  (16 B)  when colours / height band change
//   curve_indices          LineIndices                    (16 B)  on a layout change, or a re-pose
//                                                                 that changes some curve's degree
//
// against 80 B of UploadBezierData re-sent on ANY change in the other renderers. Style setters
// (Width, Cap, ...) upload nothing here - the line reads no style.
class BezierLineRenderer : public BezierRendererBase {
public:
	explicit BezierLineRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	bool NeedsCalculatedPoints() const override { return false; }
	bool NeedsBezierData() const override { return false; }

	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice&, uint32_t) override {}
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext*) override {}

	void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;

private:
	// Matches ColorData in line_common.hlsli: uint4 c0_c1_height0_height1, the heights as raw float bits.
	struct LineColorData {
		uint32_t color_begin;
		uint32_t color_end;
		float    min_height;
		float    max_height;
	};
	static_assert(sizeof(LineColorData) == 16, "LineColorData must match ColorData in line_common.hlsli");

	// Matches Indices in line_common.hlsli: uint4 first_last_first_bez_last_bez - the curve's first and
	// last SAMPLE index (inclusive), then its control-point range in curve_control_points as
	// [first_bez, end_bez) - END EXCLUSIVE, so end_bez - first_bez = power + 1.
	struct LineIndices {
		uint32_t first_index;
		uint32_t last_index;
		uint32_t first_bez;
		uint32_t end_bez;
	};
	static_assert(sizeof(LineIndices) == 16, "LineIndices must match Indices in line_common.hlsli");

	std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_control_points; // t3
	std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_colors;         // t5
	std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_indices;        // t6

	// Staging vectors, kept between uploads so a per-frame re-pose does not allocate.
	std::vector<DirectX::XMFLOAT3> control_point_scratch;
	std::vector<LineColorData>     color_scratch;
	std::vector<LineIndices>       index_scratch;
	// Degree of each curve as last uploaded - a position upload that sees one change re-sends the indices.
	std::vector<uint8_t>           uploaded_powers;
};
