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

// Fastest PatternCenterBound loop measured so far (all renderers), in ns per curve; infinity when
// nothing has been measured since the last reset.
double PatternBoundMinTimeNs();
void ResetPatternBoundMinTime();

// The per-sample / per-segment layout buffers AllocateBuffers builds. Each renderer asks only for the
// ones its passes read (BezierRendererBase::LayoutBuffers), so none is built, uploaded or kept for
// nothing.
enum LayoutBufferBits : uint8_t {
	LayoutSampleMap   = 1u << 0, // bezier_data_map: curve per SAMPLE - the point passes
	LayoutSegmentMap  = 1u << 1, // segment_map: curve + continuation per SEGMENT - the strip and line draws
	LayoutChainBegins = 1u << 2, // curve_begins: a bit per sample where a chain starts - the segmented scan
	LayoutAll         = LayoutSampleMap | LayoutSegmentMap | LayoutChainBegins
};

// Which part of the curve data a BezierCurve setter changed. UploadCurves() re-uploads only the
// buffers whose part is dirty.
// DirtyLayout is never raised by a setter: it is what AllocateBuffers() adds, because sample ranges
// (first/last index) only move when the layout does.
enum CurveDirtyBits : uint8_t {
	DirtyPositions  = 1u << 0, // control_points()
	DirtyColors     = 1u << 1, // colors(), HeightRange()
	DirtyStyles     = 1u << 2, // Width(), Cap(), Join(), DashLength(), Spacing(), Dot()
	DirtyLayout     = 1u << 3, // sample ranges - after a resize / relayout only
	DirtyAll        = DirtyPositions | DirtyColors | DirtyStyles | DirtyLayout
};

// Bind nullptr to each listed slot: SRVs of `stage`, or compute UAVs. Every pass (draw, compute, the
// scans) unbinds exactly the slots it bound once it is done, so the next pass starts from clean slots
// and a buffer can switch between SRV and UAV. Both take an array (span) or a braced list:
//   UnbindShaderResources(context, ShaderStage::Vertex, { 3, 4, 5 });
//   UnbindUnorderedAccessViews(context, { 0 });
void UnbindShaderResources(Axodox::Graphics::GraphicsDeviceContext* context, Axodox::Graphics::ShaderStage stage,
	std::span<const uint32_t> slots);
void UnbindShaderResources(Axodox::Graphics::GraphicsDeviceContext* context, Axodox::Graphics::ShaderStage stage,
	std::initializer_list<uint32_t> slots);
void UnbindUnorderedAccessViews(Axodox::Graphics::GraphicsDeviceContext* context, std::span<const uint32_t> slots);
void UnbindUnorderedAccessViews(Axodox::Graphics::GraphicsDeviceContext* context, std::initializer_list<uint32_t> slots);

// Common part of the four renderers (line, solid, patterned, dots): the curve list, the sample layout,
// the camera constants and the per-curve upload.
//
// The curve data is split by how often it changes into three buffers, each re-uploaded only when its
// part is dirty (see CurveDirtyBits), plus the renderer's own style buffer:
//
//   curve_control_points   one 3x4 matrix per curve, rows x/y/z, columns = the control points P0..P3
//                          (48 B)  when re-posed
//   curve_colors           UploadColorData        (16 B)  when colours / the height band change
//   curve_indices          UploadSampleRange      ( 8 B)  only when the layout changes
//   curve_styles           the renderer's own     through UploadStyles(), only when a style setter fired
//
// The CPU keeps the curves in exactly that GPU form, one parallel vector per upload (cubic_points,
// color_data, style_words, ...), all indexed by curve. Setters convert on the way in - a line or
// quadratic is raised to cubic, a colour is packed to 8 bits per channel, a width is rounded to a
// whole pixel and merged with the caps and join into one word - so an upload is a plain copy of the
// vector and nothing is ever converted twice. The getters hand back what is stored (the quantised
// values), not what was passed in.
//
// The control points are one 3x4 matrix per curve, so a shader finds them from the curve index alone
// and never waits on curve_indices to know where to read. The shaders evaluate them in Bernstein form
// as the matrix times a column of weights,
//
//     P(t) = [P0 P1 P2 P3] * (s^3, 3 s^2 t, 3 s t^2, t^3),    s = 1 - t
//
// whose weights are exactly 0 / 1 at the two ends, so P(0) == P0 and P(1) == P3 bit-for-bit and a
// merged joint is exact from both sides.
//
// The sample ranges are the curve's first and last SAMPLE indices, both inclusive. They are disjoint
// and back to back, merged or not: curve i owns [first, first + resolution - 1] and the next curve
// starts right after it, so every consumer that maps a sample to t as (i - first) / (last - first)
// gets t = 0 and t = 1 at the curve's two ends.
//
// The strip and line draws go by SEGMENT, not by sample: one instance (or line) per entry of
// segment_map, which lists each curve's resolution - 1 segments back to back and nothing for the step
// from one curve's last sample to the next curve's first - between two chains that step would join
// two strokes, inside a chain it joins the two copies of a merged joint (see
// BezierData::merge_with_previous). So no draw ever has a bridge to skip. Since every curve before
// curve c has dropped exactly one sample that way, segment s of curve c starts on sample s + c.
//
// The HLSL side of the structs lives in the *_common.hlsli files - keep them in step.
class BezierRendererBase {
	friend class BezierCurve;
public:
	explicit BezierRendererBase(const Axodox::Graphics::GraphicsDevice& device);
	// Detaches every handle still alive (they become !valid() and their own destructors then do
	// nothing), so a renderer may be destroyed before or after the handles pointing into it.
	virtual ~BezierRendererBase();

	BezierRendererBase(const BezierRendererBase&) = delete;
	BezierRendererBase& operator=(const BezierRendererBase&) = delete;

	// One profiler frame per Draw() call. Every renderer records the same metric names so they can be
	// compared row by row; a renderer simply has no entry for a stage it does not have:
	//
	//   update   cpu   buffer resize + curve/style upload (UpdateBuffers)
	//   calc     gpu   the point pass compute shader      (patterned and dots only)
	//   scan     gpu   the segmented prefix sum           (patterned and dots only)
	//   pattern  gpu   pattern/dot ini + calc             (patterned and dots only)
	//   draw     gpu   binds + the draw call
	//   total    cpu   the whole Draw() call
	//   total    gpu   the whole Draw() call
	Profiler profiler;

	// The returned handle OWNS the curve: when it is destroyed (or Remove()d, or move-assigned over)
	// the curve leaves the renderer. Discarding the return value therefore removes the curve again on
	// the next UpdateBuffers - hence [[nodiscard]].
	[[nodiscard]] BezierCurve Add(const BezierData& curve);
	// Same as curve.Remove(). Does nothing for a handle that is invalid or belongs to another renderer.
	void Remove(BezierCurve& curve);
	// Live curves - removed ones are excluded immediately, even before the next compaction.
	size_t Count() const { return owners.size() - removed_count; }

	void SetViewport(float width, float height);

	// The number of pattern centres (or dots) the scene can possibly produce, computed on the CPU
	// from the curve data alone - no compute pass, no readback, no lag. A cubic's arc length never
	// exceeds its control polygon |P0P1| + |P1P2| + |P2P3|, and the polyline the point pass actually
	// measures is a chord sum that never exceeds the true arc length. The centres of a chain sit on one
	// grid of world distances n * spacing, and a curve owns the window of that grid inside its own arc
	// span - floor(arcEnd / spacing) - floor(arcBegin / spacing), at most floor(own arc / spacing) + 1.
	// So
	//
	//     floor(polygon / spacing) + 1  >=  centres the curve can produce
	//
	// and sizing the pattern buffer from this sum is always sufficient. 0 for renderers with no
	// pattern buffer (line, solid).
	uint32_t PatternBound() const { return pattern_upper_bound; }

	// How much was actually allocated from the bound above. 0 where a renderer has no pattern buffer
	// at all. There is deliberately no PatternCount(): the GPU's exact total lives in the slot
	// ParalellScan appends past the last curve and is read there by the shaders that need it.
	virtual uint32_t PatternCapacity() const { return 0u; }

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

	// The curves, one parallel vector per kind of data, all indexed by curve (see the class comment).
	// Dense and in Add order, which is also the chain order (see merge_with_previous). Removal is
	// lazy: Remove() only nulls the curve's `owners` slot and raises need_resize, and the next
	// UpdateBuffers compacts every vector in one pass (CompactRemoved), rewriting the index held by
	// every surviving handle. Bulk removal is therefore O(n) once, not O(n) per curve, and a handle's
	// index stays valid right up to that compaction. Everything that runs after UpdateBuffers - every
	// Draw pass - sees fully compacted vectors, so TotalCurves() is the live count there.
	//
	// cubic_points: one 3x4 matrix per curve, row = coordinate (x, y, z), column i = control point Pi,
	// so P3 sits in the fourth (.w) column. The shaders read it as a row_major float3x4 and evaluate
	// mul(P, BernsteinWeights(t)).
	//
	// style_words: the per-curve style word the patterned, solid and dot shaders read,
	//     width << 24 | cap_front << 16 | cap_back << 8 | join
	// The width takes the top byte, so it is rounded to a WHOLE pixel and clamped to [0, 255]. The low
	// three bytes are the CapCapJoin the pixel shaders unpack. The dot renderer has no joins and masks
	// the low byte off.
	std::vector<DirectX::XMFLOAT3X4> cubic_points; // 1 per curve: columns P0..P3, uploaded as curve_control_points
	std::vector<UploadColorData>   color_data;    // uploaded as curve_colors
	std::vector<uint32_t>          style_words;   // width << 24 | cap_front << 16 | cap_back << 8 | join
	std::vector<float>             spacings;
	std::vector<float>             dash_lengths;
	std::vector<uint32_t>          resolutions;
	std::vector<uint8_t>           merged;        // merge_with_previous, see BezierData
	std::vector<BezierCurve*>      owners;        // owners[i] is the handle owning curve i; nullptr = removed
	uint32_t removed_count = 0;

	// Curve slots, removed-but-not-yet-compacted ones included.
	size_t TotalCurves() const { return owners.size(); }

	// See PatternBound(). Recomputed by UpdatePatternBound().
	uint32_t pattern_upper_bound = 0;

	bool need_resize = true;
	bool need_upload = false;
	// CurveDirtyBits: which parts the setters since the last upload touched. Raised together with
	// need_upload, cleared together with it.
	uint8_t upload_parts = 0;

	uint32_t total_points = 0;
	// total_points - curve count: every curve has one segment fewer than samples. What the strip and
	// line draws are sized by.
	uint32_t total_segments = 0;

	uint32_t points_allocated = 0;
	uint32_t curves_allocated = 0;

	// The layout buffers, each built only if LayoutBuffers() asks for it (null otherwise).
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data_map; // One uint32 curve index per point
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

	// Whether curve `index` starts a new stroke: curve 0 always does, any other curve unless it is
	// merged into the one before it. The single place that turns merge_with_previous into chains.
	bool IsChainStart(size_t index) const;

	bool UpdateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void UploadCameraData(const DirectX::XMMATRIX& view_proj, Axodox::Graphics::GraphicsDeviceContext* context);

	// PatternBound() over the live curves, from the cubic control polygons and spacings. Needs
	// positions AND spacings, so a pattern renderer redoes it when either changed.
	void UpdatePatternBound();

	// Opens and closes the profiler frame and the "total" metric. Every Draw() override must call
	// BeginDraw() first and EndDraw() on every exit path, early returns included.
	void BeginDraw();
	void EndDraw();

	// Whether the shared curve buffers exist yet - the Draw() early-out test.
	bool HasCurveData() const { return curve_control_points != nullptr; }
	void BindCurveData(Axodox::Graphics::GraphicsDeviceContext* context,
		uint32_t control_point_slot, uint32_t color_slot, uint32_t index_slot);

	// Fills the per-curve GPU data. `parts` is a CurveDirtyBits mask: DirtyAll straight after a
	// (re)layout, otherwise whatever the setters touched. Each buffer is rewritten whole when its part
	// is dirty - StructuredBuffer::Upload maps with WRITE_DISCARD, which leaves anything not written
	// undefined - so the split is per buffer, not per curve. A renderer that needs more on top of this
	// overrides it and calls this version first.
	virtual void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts);

	// Which layout buffers (LayoutBufferBits) this renderer's passes read; AllocateBuffers builds only
	// those. Everything by default - the patterned renderer reads all three.
	virtual uint8_t LayoutBuffers() const { return LayoutAll; }

	// Renderer-specific buffers, on top of the shared ones above.
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
