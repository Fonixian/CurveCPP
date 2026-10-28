#pragma once
#include "BezierData.h"
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <span>
#include <initializer_list>
#include <cassert>
#include "pipeline.h"
#include "profiler.h"

struct CameraDataBuffer {
	DirectX::XMFLOAT4X4 VP;
	DirectX::XMFLOAT2 wh;
	uint32_t TotalPointCount;
	uint32_t TotalCurveCount;
};

constexpr uint32_t maxPatternCount = 4'000'000u; // To avoid allocating giant buffer when spacing is near zero.
constexpr uint32_t maxElementCount = 1'200'000u; // For the scans, because they preallocate thier buffers.

uint32_t FitCapacity(uint32_t allocated, uint32_t required); // Get the buffer size based on the current and required size.

constexpr uint32_t GroupCount(uint32_t items, uint32_t group_size) { return (items + group_size - 1u) / group_size; }

// Fastest PatternCenterBound loop measured so far, in ns per curve (infinity if none since the reset).
double PatternBoundMinTimeNs();
void ResetPatternBoundMinTime();

// Which part of the curve data changed; UploadCurves() re-uploads only the dirty buffers.
// DirtyLayout is never set by a setter, only by AllocateBuffers() (sample ranges move with the layout).
enum CurveDirtyBits : uint8_t {
	DirtyPositions  = 1u << 0, // control_points()
	DirtyColors     = 1u << 1, // colors(), HeightRange()
	DirtyStyles     = 1u << 2, // Width(), Cap(), Join(), DashLength(), Spacing(), Dot()
	DirtyLayout     = 1u << 3, // sample ranges - after a resize / relayout only
	DirtyAll        = DirtyPositions | DirtyColors | DirtyStyles | DirtyLayout
};

void UnbindShaderResources(Axodox::Graphics::GraphicsDeviceContext* context, Axodox::Graphics::ShaderStage stage,
	std::span<const uint32_t> slots);
void UnbindShaderResources(Axodox::Graphics::GraphicsDeviceContext* context, Axodox::Graphics::ShaderStage stage,
	std::initializer_list<uint32_t> slots);
void UnbindUnorderedAccessViews(Axodox::Graphics::GraphicsDeviceContext* context, std::span<const uint32_t> slots);
void UnbindUnorderedAccessViews(Axodox::Graphics::GraphicsDeviceContext* context, std::initializer_list<uint32_t> slots);

// Common part of the four renderers (line, solid, patterned, dots): curve list, sample layout, camera and
// per-curve upload. Curve data lives on the CPU already in GPU form, one vector per buffer (see the members).
//
// Per-curve buffers, each re-uploaded only when its CurveDirtyBits part changes:
//   curve_control_points   3x4 matrix, rows x/y/z, columns P0..P3 (48 B)   - DirtyPositions
//   curve_colors           UploadColorData (16 B)                          - DirtyColors
//   curve_indices          UploadSampleRange (8 B)                         - DirtyLayout
//   curve_styles           renderer's own, via UploadStyles()              - DirtyStyles
//
// Setters convert on the way in (raise to cubic, 8-bit colours, whole-px width), so getters return the
// quantised values and uploads are plain copies.
// Shaders evaluate P(t) = [P0 P1 P2 P3] * (s^3, 3s^2t, 3st^2, t^3), s = 1 - t: exact at t = 0 / 1, so
// merged joints match bit-for-bit from both sides.
// Sample ranges are inclusive and back to back: curve i owns [first, first + resolution - 1], t = 0..1.
// Strip and line draws go per SEGMENT (segment_map): resolution - 1 per curve, no bridge between curves,
// so segment s of curve c starts on sample s + c.
//
// The HLSL side of the structs lives in the *_common.hlsli files - keep them in step.
class BezierRendererBase {
	friend class BezierCurve;
public:
	explicit BezierRendererBase(const Axodox::Graphics::GraphicsDevice& device);
	virtual ~BezierRendererBase();

	BezierRendererBase(const BezierRendererBase&) = delete;
	BezierRendererBase& operator=(const BezierRendererBase&) = delete;

	Profiler profiler;

	[[nodiscard]] BezierCurve Add(const BezierData& curve); // The handle owns the curve
	void Remove(BezierCurve& curve); // Same as curve.Remove();
	
	void SetViewport(float width, float height);

	size_t Count() const { return owners.size() - removed_count; }
	uint32_t PatternBound() const { return pattern_upper_bound; } // CPU calculated upper bound

	uint32_t PatternCapacity() const { return patterns_allocated; }

	virtual void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) = 0;

protected:
	// Matches ColorData in the shaders: uint4 c0_c1_height0_height1, the heights as raw float bits.
	struct UploadColorData {
		uint32_t color_begin;
		uint32_t color_end;
		float    min_height;
		float    max_height;
	};
	static_assert(sizeof(UploadColorData) == 16, "UploadColorData must match ColorData in the shaders");

	// Matches Indices in the shaders: uint2 first_last - the curve's first and last SAMPLE index.
	struct UploadSampleRange {
		uint32_t first_index;
		uint32_t last_index;
	};
	static_assert(sizeof(UploadSampleRange) == 8, "UploadSampleRange must match Indices in the shaders");

	// Curve data
	std::vector<DirectX::XMFLOAT3X4> cubic_points; // 1 per curve: rows x/y/z, columns P0..P3 (row_major float3x4 in HLSL)
	std::vector<UploadColorData>   color_data;    // uploaded as curve_colors
	std::vector<uint32_t>          style_words;   // width << 24 | cap_front << 16 | cap_back << 8 | join (width in whole px, dots mask join)
	std::vector<float>             spacings;
	std::vector<float>             dash_lengths;
	std::vector<uint32_t>          resolutions;
	std::vector<uint8_t>           merged;        // merge_with_previous, see BezierData
	std::vector<BezierCurve*>      owners;        // owners[i] is the handle owning curve i; nullptr = removed
	uint32_t removed_count = 0;

	size_t TotalCurves() const { return owners.size(); }

	// See PatternBound(). Recomputed by UpdatePatternBound().
	uint32_t pattern_upper_bound = 0;

	bool need_resize = true;
	bool need_upload = false;
	uint8_t upload_parts = 0; // CurveDirtyBits touched since the last upload; set and cleared with need_upload

	uint32_t total_points = 0;
	uint32_t total_segments = 0; // total_points - curve count; sizes the strip and line draws

	uint32_t points_allocated = 0;
	uint32_t curves_allocated = 0;
	uint32_t patterns_allocated = 0; // pattern centre / dot buffer, sized from PatternBound(); stays 0 for line and solid

	// Layout buffers: created by the renderer's AllocatePointBuffers through the Allocate* helpers below;
	// null = this renderer does not use it, and AllocateBuffers then neither builds nor uploads it.
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data_map; // One uint32 curve index per point - the point passes
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   segment_map;     // One uint32 per segment: curve | SegmentHasPrev | SegmentHasNext
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_control_points;
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_colors;
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_indices;
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_styles;    // The renderer's own, see AllocateStyleBuffer

	std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_begins;   // read-only on the GPU: the segmented scan and curve_pattern_calc only sample it

	Axodox::Graphics::ComputeShader* calc_points = nullptr;

	CameraDataBuffer camera_cb_data{};
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> viewport_data; // Camera / viewport / counts
	Pipeline curve_draw;

	// Curve 0 and every unmerged curve start a chain; the one place merge_with_previous becomes chains.
	bool IsChainStart(size_t index) const;

	bool UpdateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void UploadCameraData(const DirectX::XMMATRIX& view_proj, Axodox::Graphics::GraphicsDeviceContext* context);

	// Recomputes PatternBound(); needs positions and spacings, so redo it when either changes.
	void UpdatePatternBound();

	// Open / close the profiler frame; every Draw() calls BeginDraw() first and EndDraw() on every exit.
	void BeginDraw();
	void EndDraw();

	// Whether the shared curve buffers exist yet - the Draw() early-out test.
	bool HasCurveData() const { return curve_control_points != nullptr; }
	void BindCurveData(Axodox::Graphics::GraphicsDeviceContext* context,
		uint32_t control_point_slot, uint32_t color_slot, uint32_t index_slot);

	// Rewrites each buffer whose CurveDirtyBits part is in `parts` (whole buffer, WRITE_DISCARD).
	// Overrides call this version first.
	virtual void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts);

	// Create one layout buffer each; call them from AllocatePointBuffers for the buffers the renderer reads.
	void AllocateSampleMap(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required);   // bezier_data_map
	void AllocateSegmentMap(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required);  // segment_map
	void AllocateChainBegins(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required); // curve_begins

	// Renderer-specific buffers, on top of the shared ones above. AllocatePointBuffers also creates the
	// layout buffers the renderer reads (it runs before AllocateBuffers builds and uploads them).
	virtual void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) {}
	virtual void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) {}
	virtual void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) = 0;
	// Called from UploadCurves when DirtyStyles is set.
	virtual void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) = 0;

private:
	void CompactRemoved();
	void AllocateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);

	// Staging vector for the sample ranges, kept between uploads so a relayout does not allocate.
	std::vector<UploadSampleRange> index_scratch;
};
