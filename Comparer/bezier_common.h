#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <span>
#include <cassert>
#include "pipeline.h"
#include "profiler.h"

enum class CurveCap : uint8_t {
	Butt = 0,
	Square = 1,
	Round = 2,
	TriangleOut = 3,
	TriangleIn = 4
};

enum class CurveJoin : uint8_t {
	Round = 0,
	Square = 1
};

struct BezierData {
	// Control points, always CUBIC: Linear() and Quadratic() raise to cubic when they build the struct,
	// exactly as the handle's control_points() overloads do, so a degree never travels any further.
	DirectX::XMFLOAT3 P0 = { 0.f, 0.f, 0.f };
	DirectX::XMFLOAT3 P1 = { 0.f, 0.f, 0.f };
	DirectX::XMFLOAT3 P2 = { 0.f, 0.f, 0.f };
	DirectX::XMFLOAT3 P3 = { 0.f, 0.f, 0.f };
	// Colors
	DirectX::XMFLOAT3 C0 = { 1.f, 1.f, 1.f };
	DirectX::XMFLOAT3 C1 = { 1.f, 1.f, 1.f };
	float min_height = 0.f;
	float max_height = 0.f;
	float width = 2.f;
	CurveCap cap_front = CurveCap::Butt;
	CurveCap cap_back = CurveCap::Butt;
	CurveJoin join = CurveJoin::Round;
	float dash_length = 0.f;
	float spacing = 0.f;
	unsigned resolution = 64u;
	// Merged curves: when true, this curve continues the stroke of the curve Added directly before it
	// in the same renderer, so the two share one arc length, one pattern grid and one pair of end
	// caps. A CHAIN is a run of consecutive curves in which every curve after the first has this set.
	// Chains are contiguous by construction - the segmented scan's segments are runs of points
	// between begin bits, so only the previous index can ever be merged into. Ignored on curve 0.
	//
	// Every curve in a chain keeps its own width, colour, join and spacing. The front cap only ever
	// fires on the chain's first curve and the back cap on its last one - the vertex shaders find the
	// terminus from the chain's begin bits (curve_begins).
	//
	// ASSUMES the curve starts exactly where the previous one ends (its P0 == the previous P3; a debug
	// build asserts it). Merged or not, every curve owns exactly `resolution` samples, at
	// t = j / (resolution - 1), so merging never moves a sample: the joint is sampled twice, as the
	// previous curve's last sample (its t = 1) and as this curve's first (its t = 0). The zero-length
	// step between those two copies is never drawn - segment_map has no entry for it, just as it has
	// none for the step between two chains - and a neighbour across the joint steps over the duplicate
	// (sample i - 2 / i + 3 instead of i - 1 / i + 2).
	bool merge_with_previous = false;
};

BezierData Linear(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1);
BezierData Quadratic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2);
BezierData Cubic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2, DirectX::XMFLOAT3 P3);

struct CameraDataBuffer {
	DirectX::XMFLOAT4X4 VP;
	DirectX::XMFLOAT2 wh;
	uint32_t TotalPointCount;
	uint32_t TotalCurveCount;
};

// Ceiling on the CPU-side pattern bound, so a curve with a near-zero spacing asks for a buffer of a
// sane size rather than one of a few billion entries. Past this the calc shaders clamp and the tail
// of the pattern simply is not drawn.
constexpr uint32_t maxPatternCount = 4'000'000u;

// Capacity every scan instance is built for. A curve is worth at least one point, so it bounds the
// per-curve scans (centre / dot offsets) as well as the per-point ones.
constexpr uint32_t maxElementCount = 1'200'000u;

// Capacity policy shared by every over-allocated GPU buffer (points, curves, pattern / dot slots).
// Returns the capacity the buffer SHOULD have for `required` live elements, given what it has now:
//   - grows to next_pow2(required) as soon as it no longer fits,
//   - shrinks to next_pow2(required) once usage has fallen to a quarter of the allocation or less,
//   - otherwise keeps `allocated`.
// The factor-4 gap between the two thresholds is the hysteresis: a count hovering around a power of
// two (an animated pattern bound, a batch added then removed) cannot make the buffer flip between two
// sizes every frame. Reallocate exactly when the result differs from `allocated`.
uint32_t FitCapacity(uint32_t allocated, uint32_t required);

// Thread groups needed to cover `items` threads of `group_size` each.
constexpr uint32_t GroupCount(uint32_t items, uint32_t group_size) { return (items + group_size - 1u) / group_size; }

uint32_t PackFloat3ToR8G8B8A8(const DirectX::XMFLOAT3& color);
DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);
DirectX::XMFLOAT3 UnpackFloat3FromR8G8B8A8(uint32_t color);

// Raise a line / quadratic to the four cubic control points the renderers store.
void LinearToCubic(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, DirectX::XMFLOAT3 (&cubic)[4]);
void QuadraticToCubic(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2, DirectX::XMFLOAT3 (&cubic)[4]);

// A width as the style word's top byte: rounded to a whole pixel and clamped to [0, 255].
uint32_t PackWidthByte(float width);

// The per-curve style word the patterned, solid and dot shaders read:
//
//     width << 24 | cap_front << 16 | cap_back << 8 | join
//
// The width takes the top byte, so it is rounded to a WHOLE pixel and clamped to [0, 255]. The low
// three bytes are the CapCapJoin the pixel shaders unpack. The dot renderer has no joins and masks the
// low byte off.
uint32_t PackWidthCapCapJoin(const BezierData& bez);

// One curve's share of PatternBound(): floor(control polygon / spacing) + 1, clamped to
// maxPatternCount, or 0 when spacing <= 0. Takes the CUBIC control points as stored, since the
// polygon of a raised curve is what bounds the arc length the point pass measures.
uint64_t PatternCenterBound(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2, const DirectX::XMFLOAT3& p3, float spacing);

// Whether two curve ends coincide closely enough to be merged. Relative, so it holds at any scale;
// only used by the merge asserts in the curve upload.
bool EndpointsMeet(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b);

// A cubic as the renderer stores and uploads it: one 3x4 matrix, row = coordinate (x, y, z), column i =
// control point Pi, so P3 sits in the fourth (.w) column. The shaders read it as a row_major float3x4
// and evaluate mul(P, BernsteinWeights(t)).
DirectX::XMFLOAT3X4 ToControlMatrix(const DirectX::XMFLOAT3 (&cubic)[4]);
inline DirectX::XMFLOAT3 ControlPoint(const DirectX::XMFLOAT3X4& m, int i) { return { m.m[0][i], m.m[1][i], m.m[2][i] }; }

// segment_map entries: the curve index in the low 30 bits, then whether the stroke goes on past the
// segment's start (into the segment before it - same curve, or the last one of a merged previous
// curve) and past its end. Same constants in solid_common / curve_common / line_common.hlsli.
constexpr uint32_t SegmentHasPrev   = 1u << 30;
constexpr uint32_t SegmentHasNext   = 1u << 31;
constexpr uint32_t SegmentCurveMask = SegmentHasPrev - 1u;

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

void ClearComputeBindings(Axodox::Graphics::GraphicsDeviceContext* context);
void ClearDrawBindings(Axodox::Graphics::GraphicsDeviceContext* context);

class BezierCurve;

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

// An OWNING handle to one curve in one renderer - move-only, like a unique_ptr. The curve is removed
// from its renderer when the handle is destroyed, when Remove() is called, or when another handle is
// move-assigned into it. A default-constructed or moved-from handle owns nothing (!valid()), and
// every accessor below asserts valid().
//
// The renderer keeps a pointer back to the handle (BezierRendererBase::owners) so it can rewrite
// curve_index when removals compact the curve array; the move operations keep that pointer current,
// so handles may live anywhere - members, arrays, std::vector (reallocation moves them).
class BezierCurve {
	friend class BezierRendererBase;
protected:
	size_t curve_index = 0;
	BezierRendererBase* renderer = nullptr;

	BezierCurve(BezierRendererBase* owner, size_t index) : curve_index(index), renderer(owner) {}

	// This curve's slot in each of the renderer's vectors.
	DirectX::XMFLOAT3X4& points() const { assert(valid()); return renderer->cubic_points[curve_index]; }
	BezierRendererBase::UploadColorData& color() const { assert(valid()); return renderer->color_data[curve_index]; }
	uint32_t& style() const { assert(valid()); return renderer->style_words[curve_index]; }

	void store_points(const DirectX::XMFLOAT3 (&cubic)[4]) {
		points() = ToControlMatrix(cubic);
		touch(DirtyPositions);
	}

	// `parts` is a CurveDirtyBits mask; see there. The default keeps any caller that does not say what
	// it changed on the safe side.
	inline void touch(uint8_t parts = DirtyAll) { renderer->need_upload = true; renderer->upload_parts |= parts; }
	inline void touch_layout() { renderer->need_resize = true; }

public:
	BezierCurve() = default;
	~BezierCurve() { Remove(); }

	BezierCurve(const BezierCurve&) = delete;
	BezierCurve& operator=(const BezierCurve&) = delete;

	BezierCurve(BezierCurve&& other) noexcept;
	// Removes the curve this handle owned (if any), then takes over `other`'s.
	BezierCurve& operator=(BezierCurve&& other) noexcept;

	// Removes the curve from its renderer; the handle is invalid afterwards. Safe on an invalid handle.
	// If the curve was part of a merged chain, the chain is SPLIT there: the curve that followed it
	// starts a new stroke (its Merged() reads false from the next UpdateBuffers on). Keeping it merged
	// would carry the arc length, the pattern and the joins straight across the gap.
	void Remove();

	inline bool valid() const { return renderer != nullptr; }

	// Lines and quadratics are raised to cubic here, so the renderer only ever holds cubic points.
	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1) { DirectX::XMFLOAT3 c[4]; LinearToCubic(P0, P1, c); store_points(c); }
	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2) { DirectX::XMFLOAT3 c[4]; QuadraticToCubic(P0, P1, P2, c); store_points(c); }
	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2, const DirectX::XMFLOAT3& P3) { const DirectX::XMFLOAT3 c[4] = { P0, P1, P2, P3 }; store_points(c); }

	// The colours are stored at 8 bits per channel, so these return the quantised values.
	inline DirectX::XMFLOAT3 C0() const { return UnpackFloat3FromR8G8B8A8(color().color_begin); }
	inline DirectX::XMFLOAT3 C1() const { return UnpackFloat3FromR8G8B8A8(color().color_end); }
	inline float MinHeight() const { return color().min_height; }
	inline float MaxHeight() const { return color().max_height; }

	inline void colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1) { color().color_begin = PackFloat3ToR8G8B8A8(C0); color().color_end = PackFloat3ToR8G8B8A8(C1); touch(DirtyColors); }
	inline void HeightRange(float min, float max) { color().min_height = min; color().max_height = max; touch(DirtyColors); }

	// Width is stored as the whole pixel the shaders draw (see PackWidthByte).
	inline float Width() const { return float(style() >> 24); }
	inline CurveCap CapFront() const { return static_cast<CurveCap>((style() >> 16) & 0xFFu); }
	inline CurveCap CapBack() const { return static_cast<CurveCap>((style() >> 8) & 0xFFu); }
	inline CurveJoin Join() const { return static_cast<CurveJoin>(style() & 0xFFu); }
	inline float DashLength() const { assert(valid()); return renderer->dash_lengths[curve_index]; }
	inline float Spacing() const { assert(valid()); return renderer->spacings[curve_index]; }

	inline void Width(float value) { style() = (style() & 0x00FFFFFFu) | (PackWidthByte(value) << 24); touch(DirtyStyles); }
	inline void Cap(CurveCap front, CurveCap back) { style() = (style() & 0xFF0000FFu) | (uint32_t(front) << 16) | (uint32_t(back) << 8); touch(DirtyStyles); }
	inline void Join(CurveJoin value) { style() = (style() & ~0xFFu) | uint32_t(value); touch(DirtyStyles); }
	inline void DashLength(float value) { assert(valid()); renderer->dash_lengths[curve_index] = value; touch(DirtyStyles); }
	inline void Spacing(float value) { assert(valid()); renderer->spacings[curve_index] = value; touch(DirtyStyles); }

	inline void Dot() { Cap(CurveCap::Round, CurveCap::Round); renderer->dash_lengths[curve_index] = 0.f; }

	inline unsigned Resolution() const { assert(valid()); return renderer->resolutions[curve_index]; }
	inline void Resolution(unsigned value) { assert(valid()); renderer->resolutions[curve_index] = value; touch_layout(); }

	// See BezierData::merge_with_previous. A layout change, not a plain upload: the chain boundaries
	// live in the curve_begins bits, which are rebuilt only when the buffers are laid out. Setting the
	// value it already has does nothing, so this is safe to call every frame.
	inline bool Merged() const { assert(valid()); return renderer->merged[curve_index] != 0; }
	inline void Merged(bool value) { if (Merged() != value) { renderer->merged[curve_index] = value ? 1u : 0u; touch_layout(); } }
};
