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
	// Control points
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
	int bezier_power = 1;
	// Merged curves: when true, this curve continues the stroke of the curve Added directly before it
	// in the same renderer, so the two share one arc length, one pattern grid and one pair of end
	// caps. A CHAIN is a run of consecutive curves in which every curve after the first has this set.
	// Chains are contiguous by construction - the segmented scan's segments are runs of points
	// between begin bits, so only the previous index can ever be merged into. Ignored on curve 0.
	//
	// Every curve in a chain keeps its own width, colour, join and spacing. The front cap only ever
	// fires on the chain's first curve and the back cap on its last one - the shaders test the
	// terminus against the chain range, see UploadBezierData.
	//
	// ASSUMES the curve starts exactly where the previous one ends (its P0 == the previous P3; a debug
	// build asserts it). The two share ONE sample at the joint: it is the previous curve's last sample
	// and this curve's first, and only this curve evaluates it. So a merged curve adds resolution - 1
	// samples, not resolution, and a chain is one polyline with no duplicate point and no zero-length
	// segment at any joint - the vertex shaders need no merge-specific neighbour lookup.
	bool merge_with_previous = false;

	bool empty() const;
	void clear();
	int power() const;
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

// The four float3 slots hold the curve in the MONOMIAL (power) basis, not its control points:
//
//     P(t) = K0 + t * (K1 + t * (K2 + t * K3))
//
// Same cubic, same 80 bytes, same slots - only what the numbers mean changed. The GPU never wants
// the control points themselves, only something it can evaluate, and Horner does that in three
// fused multiply-adds per component where the Bernstein form needs three weight products and four
// scale-adds. ToPowerBasis in bezier_common.cpp does the conversion once per upload; the control
// points stay in BezierData on the CPU side, which is what PatternBound() and ToCubic() read.
//
// Whoever adds a GPU pass that genuinely needs P0..P3 (subdivision, a control-polygon bound on the
// GPU, hull rendering) has to convert back or carry them separately - the conversion is not
// invertible in-place without the Bernstein matrix.
//
// first_index / last_index are the curve's first and last SAMPLE indices. Inside a chain they overlap
// by one: a merged curve's first_index is the previous curve's last_index (the shared joint sample,
// see BezierData::merge_with_previous), so every consumer that maps a sample to t as
// (i - first) / (last - first) still gets t = 0 and t = 1 at the curve's two ends.
//
// chain_first_index / chain_last_index are the first and last sample indices of the whole chain this
// curve belongs to. For an unmerged curve they equal first_index / last_index. curve_vs.hlsl reads
// ChainLastIndex for the back-terminus test, so interior joints of a chain get no caps.
struct UploadBezierData {
	DirectX::XMFLOAT3 K0;
	int32_t  first_index;
	DirectX::XMFLOAT3 K1;
	int32_t  last_index;
	DirectX::XMFLOAT3 K2;
	uint32_t color_begin;
	DirectX::XMFLOAT3 K3;
	uint32_t color_end;
	float    min_height;
	float    max_height;
	int32_t  chain_first_index;
	int32_t  chain_last_index;
};
static_assert(sizeof(UploadBezierData) == 80, "UploadBezierData must match BezierCurveData in the shaders");

// Compute-stage b1 for the pattern/dot calc passes: how many entries the pattern buffer actually
// holds, so a shader can clamp instead of writing past the end. See PatternBound() below.
struct PatternCapacityBuffer {
	uint32_t capacity;
	uint32_t padding[3];
};

// Ceiling on the CPU-side pattern bound, so a curve with a near-zero spacing asks for a buffer of a
// sane size rather than one of a few billion entries. Past this the calc shaders clamp and the tail
// of the pattern simply is not drawn.
constexpr uint32_t maxPatternCount = 4'000'000u;

unsigned next_pow2(unsigned x);
// Capacity policy shared by every over-allocated GPU buffer (points, curves, pattern / dot slots).
// Returns the capacity the buffer SHOULD have for `required` live elements, given what it has now:
//   - grows to next_pow2(required) as soon as it no longer fits,
//   - shrinks to next_pow2(required) once usage has fallen to a quarter of the allocation or less,
//   - otherwise keeps `allocated`.
// The factor-4 gap between the two thresholds is the hysteresis: a count hovering around a power of
// two (an animated pattern bound, a batch added then removed) cannot make the buffer flip between two
// sizes every frame. Reallocate exactly when the result differs from `allocated`.
uint32_t FitCapacity(uint32_t allocated, uint32_t required);
uint32_t PackFloat3ToR8G8B8A8(const DirectX::XMFLOAT3& color);
DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);
void ToCubic(const BezierData& source, DirectX::XMFLOAT3& p0, DirectX::XMFLOAT3& p1, DirectX::XMFLOAT3& p2, DirectX::XMFLOAT3& p3);
// Cubic control points -> the monomial coefficients UploadBezierData carries. Call it on ToCubic's
// output, and only after anything that needs the control polygon itself (the pattern bound) is done.
void ToPowerBasis(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2, const DirectX::XMFLOAT3& p3,
	DirectX::XMFLOAT3& k0, DirectX::XMFLOAT3& k1, DirectX::XMFLOAT3& k2, DirectX::XMFLOAT3& k3);

// Whether two curve ends coincide closely enough to be merged (relative tolerance, so it holds at any
// scene scale). Only used by the merge asserts in the UploadCurveData implementations.
bool EndpointsMeet(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b);

void ClearComputeBindings(Axodox::Graphics::GraphicsDeviceContext* context);
void ClearDrawBindings(Axodox::Graphics::GraphicsDeviceContext* context);

class BezierCurve;

class BezierRendererBase {
	friend class BezierCurve;
public:
	explicit BezierRendererBase(const Axodox::Graphics::GraphicsDevice& device);
	// Detaches every handle still alive (they become !valid() and their own destructors then do
	// nothing), so a renderer may be destroyed before or after the handles pointing into it.
	virtual ~BezierRendererBase();

	BezierRendererBase(const BezierRendererBase&) = delete;
	BezierRendererBase& operator=(const BezierRendererBase&) = delete;

	// One profiler frame per Draw() call. Every renderer records the same metric names so the three
	// can be compared row by row, a renderer simply has no entry for a stage it does not have:
	//
	//   update   cpu   buffer resize + curve/style upload (UpdateBuffers)
	//   calc     gpu   the point pass compute shader    (not in the solid renderer)
	//   scan     gpu   the segmented prefix sum          (not in the solid renderer)
	//   pattern  gpu   pattern/dot ini + calc            (not in the solid renderer)
	//                 no longer includes a CPU readback, so it stays flat across a scene change
	//   draw     gpu   binds + DrawInstanced
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
	size_t Count() const { return curves.size() - removed_count; }

	void SetViewport(float width, float height);

	// The number of pattern centres (or dots) the scene can possibly produce, computed on the CPU
	// from the curve data alone - no compute pass, no readback, no lag. A cubic's arc length never
	// exceeds its control polygon |P0P1| + |P1P2| + |P2P3|, and the polyline the point pass actually
	// measures is a chord sum that never exceeds the true arc length, so
	//
	//     floor(polygon / spacing) + 1  >=  floor(sampled arc / spacing) + 1
	//
	// which is exactly what curve_pattern_ini.hlsl / dot_ini.hlsl USED to count. Both now take a
	// window of a chain-global centre grid instead - floor(arcEnd / spacing) - floor(arcBegin /
	// spacing) - so the pattern runs continuously across merged curves. The bound still holds: that
	// difference is at most floor(own arc / spacing) + 1, which is the same quantity again. Sizing
	// the pattern buffer from this is therefore always sufficient, and it is the reason the GPU
	// count no longer has to come back to the CPU mid-frame. 0 for the solid renderer, which has no
	// pattern buffer.
	uint32_t PatternBound() const { return pattern_upper_bound; }

	// How much was actually allocated from the bound above. 0 where a renderer has no pattern buffer
	// at all. There is deliberately no PatternCount(): the GPU's exact total now lives in the slot
	// ParalellScan appends past the last curve and is read there by the shaders that need it, so
	// mirroring it back to the CPU would mean adding a dispatch purely to feed a diagnostic.
	virtual uint32_t PatternCapacity() const { return 0u; }

	virtual void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) = 0;

protected:
	// Dense and in Add order, which is also the chain order (see merge_with_previous). Removal is
	// lazy: Remove() only nulls the curve's `owners` slot and raises need_resize, and the next
	// UpdateBuffers compacts both vectors in one pass (CompactRemoved), rewriting the index held by
	// every surviving handle. Bulk removal is therefore O(n) once, not O(n) per curve, and a handle's
	// index stays valid right up to that compaction. Everything that runs after UpdateBuffers - every
	// Draw pass - sees a fully compacted `curves`, so curves.size() is the live count there.
	std::vector<BezierData> curves;
	std::vector<BezierCurve*> owners;  // owners[i] is the handle owning curves[i]; nullptr = removed
	uint32_t removed_count = 0;

	// Recomputed alongside every curve upload; see PatternBound().
	uint32_t pattern_upper_bound = 0;

	bool need_resize = true;
	bool need_upload = false;

	uint32_t total_points = 0;

	uint32_t points_allocated = 0;
	uint32_t curves_allocated = 0;

	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data;     // Curve definitions, see AllocateCurveData
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data_map; // One uint32 curve index per point
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_styles;

	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> calculated_points;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> curve_begins;

	Axodox::Graphics::ComputeShader* calc_points = nullptr;

	CameraDataBuffer camera_cb_data{};
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> viewport_data; // Camera / viewport / counts
	Pipeline curve_draw;

	// Whether curve `index` starts a new stroke: curve 0 always does, any other curve unless it is
	// merged into the one before it. The single place that turns merge_with_previous into chains.
	bool IsChainStart(size_t index) const;

	bool UpdateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void UploadCameraData(const DirectX::XMMATRIX& view_proj, Axodox::Graphics::GraphicsDeviceContext* context);

	// Opens and closes the profiler frame and the "total" metric. Every Draw() override must call
	// BeginDraw() first and EndDraw() on every exit path, early returns included.
	void BeginDraw();
	void EndDraw();

	// Whether this renderer wants `calculated_points` - one float4 per sample point, world position
	// plus packed colour. Only the patterned renderer does now. The dot renderer touches only the two
	// samples bracketing each dot and dot_vert evaluates those itself; the solid renderer has no
	// compute pass at all and solid_vert evaluates B, C and the neighbour per vertex. For either,
	// allocating the buffer would cost 16 bytes per sample point that nothing ever reads.
	virtual bool NeedsCalculatedPoints() const { return true; }

	virtual void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) {}
	virtual void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) {}
	// A renderer that folds its style into its own curve record (the solid one) has no style buffer
	// and leaves these two empty.
	virtual void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) {}
	virtual void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) {}

	// The curve-definition buffer (`bezier_data`) and what goes into it. The defaults are the cubic
	// monomial UploadBezierData the patterned and dot renderers read. The solid renderer overrides
	// both: it uploads the control points as given (2, 3 or 4 of them) plus a smaller per-curve
	// record, see bezier_solid.h. An override of UploadCurveData must set pattern_upper_bound itself.
	virtual void AllocateCurveData(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required);
	virtual void UploadCurveData(Axodox::Graphics::GraphicsDeviceContext* context);

private:
	void CompactRemoved();
	void AllocateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
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

	BezierData& data() { assert(valid()); return renderer->curves[curve_index]; }
	const BezierData& data() const { assert(valid()); return renderer->curves[curve_index]; }

	inline void touch() { renderer->need_upload = true; }
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
	// would bridge the gap with a straight segment in the renderers that follow curve_begins.
	void Remove();

	inline bool valid() const { return renderer != nullptr; }
	const BezierData& Data() const { return data(); }

	inline DirectX::XMFLOAT3 P0() const { return data().P0; }
	inline DirectX::XMFLOAT3 P1() const { return data().P1; }
	inline DirectX::XMFLOAT3 P2() const { return data().P2; }
	inline DirectX::XMFLOAT3 P3() const { return data().P3; }

	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1) { data().P0 = P0; data().P1 = P1; data().bezier_power = 1; touch(); }
	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2) { data().P0 = P0; data().P1 = P1; data().P2 = P2; data().bezier_power = 2; touch(); }
	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2, const DirectX::XMFLOAT3& P3) { data().P0 = P0; data().P1 = P1; data().P2 = P2; data().P3 = P3; data().bezier_power = 3; touch(); }

	inline int power() { return data().bezier_power; };

	inline DirectX::XMFLOAT3 C0() const { return data().C0; }
	inline DirectX::XMFLOAT3 C1() const { return data().C1; }
	inline float MinHeight() const { return data().min_height; }
	inline float MaxHeight() const { return data().max_height; }

	inline void colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1) { data().C0 = C0; data().C1 = C1; touch(); }
	inline void HeightRange(float min, float max) { data().min_height = min; data().max_height = max; touch(); }

	
	inline float Width() const { return data().width; }
	inline CurveCap CapFront() const { return data().cap_front; }
	inline CurveCap CapBack() const { return data().cap_back; }
	inline CurveJoin Join() const { return data().join; }
	inline float DashLength() const { return data().dash_length; }
	inline float Spacing() const { return data().spacing; }

	inline void Width(float value) { data().width = value; touch(); }
	inline void Cap(CurveCap front, CurveCap back) { data().cap_front = front; data().cap_back = back; touch(); }
	inline void Join(CurveJoin value) { data().join = value; touch(); }
	inline void DashLength(float value) { data().dash_length = value; touch(); }
	inline void Spacing(float value) { data().spacing = value; touch(); }

	inline void Dot() { data().cap_front = CurveCap::Round; data().cap_back = CurveCap::Round; data().dash_length = 0.f; touch(); }

	inline unsigned Resolution() const { return data().resolution; }
	inline void Resolution(unsigned value) { data().resolution = value; touch_layout(); }

	// See BezierData::merge_with_previous. A layout change, not a plain upload: the chain boundaries
	// live in the curve_begins bits, which are rebuilt only when the buffers are laid out. Setting the
	// value it already has does nothing, so this is safe to call every frame.
	inline bool Merged() const { return data().merge_with_previous; }
	inline void Merged(bool value) { if (data().merge_with_previous != value) { data().merge_with_previous = value; touch_layout(); } }
};
