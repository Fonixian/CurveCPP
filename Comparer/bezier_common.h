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
	// Always cubic: Linear() and Quadratic() raise to cubic.
	DirectX::XMFLOAT3 P0 = { 0.f, 0.f, 0.f };
	DirectX::XMFLOAT3 P1 = { 0.f, 0.f, 0.f };
	DirectX::XMFLOAT3 P2 = { 0.f, 0.f, 0.f };
	DirectX::XMFLOAT3 P3 = { 0.f, 0.f, 0.f };
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
	// Continue the stroke of the curve Added just before this one (one arc length, one pattern, caps
	// only at the chain's two ends). Requires P0 == the previous P3; the joint sample is shared, so a
	// merged curve adds resolution - 1 samples. Ignored on curve 0. In a BezierRenderer the whole
	// chain is routed by its first curve.
	bool merge_with_previous = false;
};

BezierData Linear(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1);
BezierData Quadratic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2);
BezierData Cubic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2, DirectX::XMFLOAT3 P3);

// Which backend of a BezierRenderer draws a curve.
enum class CurveKind : uint8_t {
	Solid = 0,     // BezierSolidRenderer
	Patterned = 1, // BezierPatternRenderer
	Dotted = 2     // BezierDotRenderer
};
constexpr size_t CurveKindCount = 3;
const char* CurveKindName(CurveKind kind);

// spacing 0 -> Solid, dash length 0 -> Dotted, otherwise Patterned ("0" = not above zero, NaN too).
constexpr CurveKind KindOf(float spacing, float dash_length) {
	return !(spacing > 0.f) ? CurveKind::Solid
		: !(dash_length > 0.f) ? CurveKind::Dotted
		: CurveKind::Patterned;
}
constexpr CurveKind KindOf(const BezierData& curve) { return KindOf(curve.spacing, curve.dash_length); }

struct CameraDataBuffer {
	DirectX::XMFLOAT4X4 VP;
	DirectX::XMFLOAT2 wh;
	uint32_t TotalPointCount;
	uint32_t TotalCurveCount;
};

// Cap on the pattern bound, so a near-zero spacing cannot ask for billions of slots.
constexpr uint32_t maxPatternCount = 4'000'000u;

// Capacity of every scan instance (points, and therefore curves too).
constexpr uint32_t maxElementCount = 1'200'000u;

// Grows to next_pow2(required) when it no longer fits, shrinks to it once usage drops to a quarter,
// otherwise keeps `allocated`.
uint32_t FitCapacity(uint32_t allocated, uint32_t required);

constexpr uint32_t GroupCount(uint32_t items, uint32_t group_size) { return (items + group_size - 1u) / group_size; }

uint32_t PackFloat3ToR8G8B8A8(const DirectX::XMFLOAT3& color);
DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);
DirectX::XMFLOAT3 UnpackFloat3FromR8G8B8A8(uint32_t color);

void LinearToCubic(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, DirectX::XMFLOAT3 (&cubic)[4]);
void QuadraticToCubic(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2, DirectX::XMFLOAT3 (&cubic)[4]);

// Rounded to a whole pixel, clamped to [0, 255].
uint32_t PackWidthByte(float width);

// width << 24 | cap_front << 16 | cap_back << 8 | join
uint32_t PackWidthCapCapJoin(const BezierData& bez);

// floor(control polygon / spacing) + 1, or 0 when spacing <= 0.
uint64_t PatternCenterBound(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2, const DirectX::XMFLOAT3& p3, float spacing);

// Relative tolerance; used by the merge asserts.
bool EndpointsMeet(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b);

// Which per-curve buffers need re-uploading. DirtyLayout comes only from AllocateBuffers().
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
class BezierRenderer;

// Implemented by every single renderer and by BezierRenderer, so scene code can drive either.
class CurveRenderer {
public:
	virtual ~CurveRenderer() = default;

	[[nodiscard]] virtual BezierCurve Add(const BezierData& curve) = 0;
	virtual size_t Count() const = 0;
	virtual void SetViewport(float width, float height) = 0;
	virtual void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) = 0;
};

// Shared part of the line, solid, patterned and dot renderers: curve storage, sample layout, camera
// constants and the per-curve upload. Curves are kept on the CPU in their GPU form (one vector per
// buffer), so an upload is a plain copy; each buffer is re-sent only when its CurveDirtyBits part is
// dirty. The matching HLSL structs are in the *_common.hlsli files.
class BezierRendererBase : public CurveRenderer {
	friend class BezierCurve;
	friend class BezierRenderer; // moves curves between its backends
public:
	explicit BezierRendererBase(const Axodox::Graphics::GraphicsDevice& device);
	// Detaches the remaining handles (they become !valid()).
	virtual ~BezierRendererBase();

	BezierRendererBase(const BezierRendererBase&) = delete;
	BezierRendererBase& operator=(const BezierRendererBase&) = delete;

	// Metrics: update (cpu), calc, scan, pattern, draw, total (cpu + gpu).
	Profiler profiler;

	// The handle owns the curve; dropping it removes the curve.
	[[nodiscard]] BezierCurve Add(const BezierData& curve) override;
	void Remove(BezierCurve& curve);
	size_t Count() const override { return owners.size() - removed_count; }

	void SetViewport(float width, float height) override;

	// CPU upper bound on pattern centres / dots (sum of PatternCenterBound), what the pattern buffer
	// is sized from. The control polygon bounds the arc length, so it is always enough.
	uint32_t PatternBound() const { return pattern_upper_bound; }

	// Allocated pattern slots; 0 for renderers without a pattern buffer.
	virtual uint32_t PatternCapacity() const { return 0u; }

protected:
	// ColorData in the shaders.
	struct UploadColorData {
		uint32_t color_begin;
		uint32_t color_end;
		float    min_height;
		float    max_height;
	};
	static_assert(sizeof(UploadColorData) == 16, "UploadColorData must match ColorData in the shaders");

	// Indices in the shaders: first and last sample of the curve, inclusive. A merged curve's first
	// is the previous curve's last.
	struct UploadSampleRange {
		uint32_t first_index;
		uint32_t last_index;
	};
	static_assert(sizeof(UploadSampleRange) == 8, "UploadSampleRange must match Indices in the shaders");

	// Parallel vectors indexed by curve, in Add (= chain) order. Remove() only nulls the owners slot;
	// CompactRemoved() squeezes the vectors on the next UpdateBuffers.
	std::vector<DirectX::XMFLOAT3> cubic_points;  // 4 per curve
	std::vector<UploadColorData>   color_data;
	std::vector<uint32_t>          style_words;   // PackWidthCapCapJoin
	std::vector<float>             spacings;
	std::vector<float>             dash_lengths;
	std::vector<uint32_t>          resolutions;
	std::vector<uint8_t>           merged;
	std::vector<BezierCurve*>      owners;        // nullptr = removed
	uint32_t removed_count = 0;

	// Removed-but-not-compacted slots included.
	size_t TotalCurves() const { return owners.size(); }

	uint32_t pattern_upper_bound = 0;

	bool need_resize = true;
	bool need_upload = false;
	uint8_t upload_parts = 0; // CurveDirtyBits

	uint32_t total_points = 0;

	uint32_t points_allocated = 0;
	uint32_t curves_allocated = 0;

	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data_map; // owning curve per sample
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_control_points;
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_colors;
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_indices;
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_styles;    // layout is per renderer
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_begins;    // one bit per sample, set at chain starts

	Axodox::Graphics::ComputeShader* calc_points = nullptr;

	CameraDataBuffer camera_cb_data{};
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> viewport_data;
	Pipeline curve_draw;

	bool IsChainStart(size_t index) const;

	bool UpdateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void UploadCameraData(const DirectX::XMMATRIX& view_proj, Axodox::Graphics::GraphicsDeviceContext* context);

	void UpdatePatternBound();

	// Every Draw() calls BeginDraw() first and EndDraw() on every exit path.
	void BeginDraw();
	void EndDraw();

	bool HasCurveData() const { return curve_control_points != nullptr; }
	void BindCurveData(Axodox::Graphics::GraphicsDeviceContext* context,
		uint32_t control_point_slot, uint32_t color_slot, uint32_t index_slot);

	// Re-uploads the buffers whose part is dirty, each one whole (WRITE_DISCARD). Overrides call this
	// first.
	virtual void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts);

	virtual void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) {}
	virtual void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) {}
	virtual void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) = 0;
	virtual void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) = 0;

private:
	void CompactRemoved();
	void AllocateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);

	// Appends curve `index` to `target` and re-points its handle; the slot here is freed like Remove().
	// `chain_start` clears the merge flag so a moved chain does not join target's last curve.
	void MoveCurve(size_t index, BezierRendererBase& target, bool chain_start);

	// Curves whose chain may belong in another BezierRenderer backend now (kind changed, chain split or
	// joined), for BezierRenderer::Route(). Valid until the next compaction; UpdateBuffers clears them.
	// More marks than curves switch to route_all (check every chain).
	std::vector<uint32_t> route_marks;
	bool route_all = false;
	void MarkForRouting(size_t index);

	std::vector<UploadSampleRange> index_scratch;
};

// Owning, move-only handle to one curve (like a unique_ptr): destroying it removes the curve. The
// renderer keeps a back-pointer (owners) to update curve_index, so handles can be moved freely.
class BezierCurve {
	friend class BezierRendererBase;
protected:
	size_t curve_index = 0;
	BezierRendererBase* renderer = nullptr;

	BezierCurve(BezierRendererBase* owner, size_t index) : curve_index(index), renderer(owner) {}

	DirectX::XMFLOAT3* points() const { assert(valid()); return &renderer->cubic_points[curve_index * 4u]; }
	BezierRendererBase::UploadColorData& color() const { assert(valid()); return renderer->color_data[curve_index]; }
	uint32_t& style() const { assert(valid()); return renderer->style_words[curve_index]; }

	void store_points(const DirectX::XMFLOAT3 (&cubic)[4]) {
		DirectX::XMFLOAT3* p = points();
		for (int i = 0; i < 4; ++i) p[i] = cubic[i];
		touch(DirtyPositions);
	}

	inline void touch(uint8_t parts = DirtyAll) { renderer->need_upload = true; renderer->upload_parts |= parts; }
	inline void touch_layout() { renderer->need_resize = true; }
	inline void kind_changed(CurveKind before) { if (Kind() != before) renderer->MarkForRouting(curve_index); }

public:
	BezierCurve() = default;
	~BezierCurve() { Remove(); }

	BezierCurve(const BezierCurve&) = delete;
	BezierCurve& operator=(const BezierCurve&) = delete;

	BezierCurve(BezierCurve&& other) noexcept;
	BezierCurve& operator=(BezierCurve&& other) noexcept;

	// Splits a merged chain: the next curve starts a new stroke.
	void Remove();

	inline bool valid() const { return renderer != nullptr; }

	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1) { DirectX::XMFLOAT3 c[4]; LinearToCubic(P0, P1, c); store_points(c); }
	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2) { DirectX::XMFLOAT3 c[4]; QuadraticToCubic(P0, P1, P2, c); store_points(c); }
	inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2, const DirectX::XMFLOAT3& P3) { const DirectX::XMFLOAT3 c[4] = { P0, P1, P2, P3 }; store_points(c); }

	// Getters return the stored, quantised values (8-bit colour, whole-pixel width).
	inline DirectX::XMFLOAT3 C0() const { return UnpackFloat3FromR8G8B8A8(color().color_begin); }
	inline DirectX::XMFLOAT3 C1() const { return UnpackFloat3FromR8G8B8A8(color().color_end); }
	inline float MinHeight() const { return color().min_height; }
	inline float MaxHeight() const { return color().max_height; }

	inline void colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1) { color().color_begin = PackFloat3ToR8G8B8A8(C0); color().color_end = PackFloat3ToR8G8B8A8(C1); touch(DirtyColors); }
	inline void HeightRange(float min, float max) { color().min_height = min; color().max_height = max; touch(DirtyColors); }

	inline float Width() const { return float(style() >> 24); }
	inline CurveCap CapFront() const { return static_cast<CurveCap>((style() >> 16) & 0xFFu); }
	inline CurveCap CapBack() const { return static_cast<CurveCap>((style() >> 8) & 0xFFu); }
	inline CurveJoin Join() const { return static_cast<CurveJoin>(style() & 0xFFu); }
	inline float DashLength() const { assert(valid()); return renderer->dash_lengths[curve_index]; }
	inline float Spacing() const { assert(valid()); return renderer->spacings[curve_index]; }

	inline void Width(float value) { style() = (style() & 0x00FFFFFFu) | (PackWidthByte(value) << 24); touch(DirtyStyles); }
	inline void Cap(CurveCap front, CurveCap back) { style() = (style() & 0xFF0000FFu) | (uint32_t(front) << 16) | (uint32_t(back) << 8); touch(DirtyStyles); }
	inline void Join(CurveJoin value) { style() = (style() & ~0xFFu) | uint32_t(value); touch(DirtyStyles); }
	inline CurveKind Kind() const { return KindOf(Spacing(), DashLength()); }

	inline void DashLength(float value) { const CurveKind kind = Kind(); renderer->dash_lengths[curve_index] = value; touch(DirtyStyles); kind_changed(kind); }
	inline void Spacing(float value) { const CurveKind kind = Kind(); renderer->spacings[curve_index] = value; touch(DirtyStyles); kind_changed(kind); }

	inline void Dot() { const CurveKind kind = Kind(); Cap(CurveCap::Round, CurveCap::Round); renderer->dash_lengths[curve_index] = 0.f; kind_changed(kind); }

	inline unsigned Resolution() const { assert(valid()); return renderer->resolutions[curve_index]; }
	inline void Resolution(unsigned value) { assert(valid()); renderer->resolutions[curve_index] = value; touch_layout(); }

	// A layout change; setting the current value does nothing.
	inline bool Merged() const { assert(valid()); return renderer->merged[curve_index] != 0; }
	inline void Merged(bool value) { if (Merged() != value) { renderer->merged[curve_index] = value ? 1u : 0u; touch_layout(); renderer->MarkForRouting(curve_index); } }
};
