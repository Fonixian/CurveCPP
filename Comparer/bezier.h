#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include <vector>
#include <span>
#include <memory>
#include <limits>
#include "SegmentedScan.h"
#include "pipeline.h"

// Screen-space-thick cubic bezier strokes, cut into a pattern whose centers are pinned to fixed
// WORLD-space arc lengths along each curve.
//
// This is the arc-length implementation only - a pattern center is a screen ARC LENGTH, and the
// pixel shader cuts the stroke in the curve's own arc-length frame. (The glyph implementation
// from bezier_simple.h is deliberately not carried over.)
//
// Differences from bezier_simple.h, which this is derived from:
//   - Curves are owned by the renderer and edited through BezierCurve handles. A handle setter
//     flags the renderer for the right kind of refresh; the caller never touches a dirty flag.
//   - Curves are stored in whatever degree they were given (linear / quadratic / cubic, marked by
//     NaN in the unused control points) and are raised to cubic form only while uploading.
//   - bezier_data_map holds one uint32 curve index per point instead of two packed uint16s.
//   - Buffers are over-allocated to the next power of two and reused, so growing the curve set
//     usually costs an upload rather than a reallocation.
//   - Colour can be driven by world height instead of the curve parameter (see min_height).
//
// The full algorithm, and what runs when:
//
//   [layout rebuild - curve added, or a resolution changed]
//     - CPU: sample layout (first/last point index per curve), point->curve index map,
//       per-curve segmented-scan reset flags.
//
//   [parameter upload - any other curve parameter changed]
//     - CPU: per-curve definitions (raised to cubic here) and per-curve styles.
//
//   [every frame, because screen-space arc length depends on the camera]
//     1. curve_calc_points   - evaluate each sample point, its colour, and the length of the
//                              segment that follows it, in BOTH world and screen space.
//     2. SegmentedScan x2    - exclusive per-curve prefix sum of those segment lengths, giving
//                              cumulative world and screen arc length at every sample point.
//
//   [after a rebuild or upload, right after the first point pass - counts depend on WORLD arc
//    length and world spacing, both of which are camera independent]
//     3. curve_pattern_ini   - per curve, patternCount = floor(worldArcLength / spacing) + 1, and
//                              an InterlockedAdd into a global counter to reserve [first, count)
//                              in one shared pattern buffer. The counter is read back once
//                              (GraphicsBuffer::Download) to size that buffer.
//
//   [every frame]
//     4. curve_pattern_calc  - for pattern k of a curve, binary search the WORLD prefix sum for
//                              the segment containing k * spacing, then lerp the SCREEN prefix
//                              sum with the same factor. Result: the screen-space arc length of
//                              every pattern center.
//     5. draw                - one 5-vertex triangle strip instance per point pair (joins
//                              included). The pixel shader reconstructs its own screen arc
//                              length, picks the nearest pattern center, and shades relative to
//                              the distance from it.
namespace curve
{
	enum class CurveCap : uint32_t {
		Butt = 0,
		Square = 1,
		Round = 2,
		TriangleOut = 3,
		TriangleIn = 4
	};

	enum class CurveJoin : uint32_t {
		Round = 0, // Rounded in the pixel shader
		Square = 1 // Miter/Bevel
	};

	enum class CurvePattern : uint32_t {
		Solid = 0,
		Dash = 1,
		Dot = 2
	};

	inline const DirectX::XMFLOAT3 UnusedPoint = {
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::quiet_NaN()
	};

	struct BezierData {
		// Control points. P2 and/or P3 may be UnusedPoint - see power(). The curve is kept in the
		// degree it was given and raised to cubic only when it is uploaded.
		DirectX::XMFLOAT3 P0 = { 0.f, 0.f, 0.f };
		DirectX::XMFLOAT3 P1 = { 0.f, 0.f, 0.f };
		DirectX::XMFLOAT3 P2 = UnusedPoint;
		DirectX::XMFLOAT3 P3 = UnusedPoint;
		// Colors
		DirectX::XMFLOAT3 C0 = { 1.f, 1.f, 1.f };
		DirectX::XMFLOAT3 C1 = { 1.f, 1.f, 1.f };
		// Colour gradient domain. When min_height < max_height the C0 -> C1 blend is driven by the
		// sample point's world Y remapped from [min_height, max_height] and clamped at both ends;
		// otherwise it is driven by the curve parameter t. Leaving both at 0 gives the t blend.
		float min_height = 0.f;
		float max_height = 0.f;
		// Styles
		float width = 2.f;                            // half width, in PIXELS - constant on screen
		CurveCap cap = CurveCap::Butt;                // uploaded, not implemented yet
		CurveJoin join = CurveJoin::Round;            // uploaded, not implemented yet
		CurvePattern pattern = CurvePattern::Solid;
		float spacing = 1.f;                          // distance between patterns, WORLD space
		// Curve resolution - number of sample points. Must be >= 2.
		unsigned resolution = 64u;
		int bezier_power = 1;

		bool empty() const;
		void clear();
		// 3 = cubic, 2 = quadratic, 1 = linear, -1 = not a curve (P0 or P1 missing).
		int power() const;
	};

	// A curve with defaults for everything but the control points. Set the style fields on the
	// result, or hand it straight to BezierRenderer::Add.
	BezierData Linear(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1);
	BezierData Quadratic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2);
	BezierData Cubic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2, DirectX::XMFLOAT3 P3);

	// Matches the CameraData cbuffer in every curve_*.hlsl - b0 in the compute passes, b1 in the
	// draw. The counts live here because the buffers are over-allocated, so GetDimensions() would
	// report the allocation rather than the number of live elements.
	struct CameraDataBuffer {
		DirectX::XMFLOAT4X4 VP;
		DirectX::XMFLOAT2 wh;
		uint32_t TotalPointCount;
		uint32_t TotalCurveCount;
	};

	class BezierCurve;

	class BezierRenderer {
		friend class BezierCurve;
	public:
		explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

		// Stores the curve as-is (no cubic conversion) and returns a handle to it.
		BezierCurve Add(const BezierData& curve);

		// Handle to an already added curve. Index is stable: curves are never removed.
		BezierCurve At(size_t index);
		size_t Count() const { return curves.size(); }

		// Screen-space arc length is recomputed every frame, so a viewport change needs no
		// rebuild - it just has to land in the constant buffer before the next point pass.
		void SetViewport(float width, float height);

		// Refreshes whatever the dirty flags ask for, then draws the curves.
		void Draw(const Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj);

		// Half-length of one dash, as a multiple of the stroke half-width, in pixels.
		static constexpr float DashLengthPerWidth = 4.0f;
		// Half-length of one dot, as a multiple of the stroke half-width, in pixels.
		static constexpr float DotLengthPerWidth = 1.0f;

	protected:
		std::vector<BezierData> curves;

		// Step 0a: grow the buffers if needed, then rebuild everything that depends on the sample
		// layout (index map, curve-begin flags) and upload the curve parameters too.
		void AllocateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
		// Step 0b: re-upload the per-curve definitions and styles without touching the layout.
		// This is where linear/quadratic curves are raised to cubic form.
		void UploadCurveData(Axodox::Graphics::GraphicsDeviceContext* context);
		// Steps 1-2: point positions/colours + world and screen segment lengths, then both sums.
		void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
		// Step 3: count pattern centers, reserve their ranges, size the pattern buffer.
		void CountPatternCenters(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
		// Step 4: screen-space arc length of every pattern center.
		void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

		// --- dirty flags ---------------------------------------------------------------
		// Sample layout changed (a curve was added, or a resolution changed): the point buffers
		// may have to grow and the index map / curve-begin flags have to be rebuilt.
		bool need_resize = true;
		// Any other curve parameter changed: re-upload the per-curve definition and style buffers.
		bool need_upload = false;
		// Pattern counts and ranges are stale. Set by either of the above, cleared after the
		// readback in CountPatternCenters().
		bool need_recount = false;

		uint32_t total_points = 0;     // live sample point count across every curve
		uint32_t pattern_total = 0;    // live pattern center count across every curve

		uint32_t points_allocated = 0;   // capacity of the per-point buffers, in points
		uint32_t curves_allocated = 0;   // capacity of the per-curve buffers, in curves
		uint32_t patterns_allocated = 0; // capacity of 'patterns', in pattern centers

		// --- layout-time inputs --------------------------------------------------------
		std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data;     // Curve definitions, cubic
		std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_styles;    // Width / cap / join / pattern / spacing
		std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data_map; // One uint32 curve index per point

		// --- per-frame compute results -------------------------------------------------
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> calculated_points; // Point positions and packed colours
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;         // Cumulative arc length per point (world)
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances_screen;  // Cumulative arc length per point (screen)
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> curve_begins;      // Bit-packed flags for segmented scan reset
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_counter;   // Single uint, atomically summed pattern count
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_ranges;    // Per curve: uint2(first pattern, pattern count)
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> patterns;          // One float per pattern: screen arc length of the center

		Axodox::Graphics::ComputeShader* calc_points;
		Axodox::Graphics::ComputeShader* pattern_ini;
		Axodox::Graphics::ComputeShader* pattern_calc;
		SegmentedScan scan;

		// --- curve-body draw -----------------------------------------------------------
		CameraDataBuffer camera_cb_data{};
		std::unique_ptr<Axodox::Graphics::ConstantBuffer> viewport_data; // Camera / viewport / counts
		Pipeline curve_draw; // curve_vert + curve_ps
	};
	
	class BezierCurve {
		friend class BezierRenderer;
	protected:
		size_t curve_index = 0;
		BezierRenderer* renderer = nullptr;

		BezierCurve(BezierRenderer* owner, size_t index) : curve_index(index), renderer(owner) {}

		BezierData& data() { return renderer->curves[curve_index]; }
		const BezierData& data() const { return renderer->curves[curve_index]; }

		inline void touch() { renderer->need_upload = true; }
		inline void touch_layout() { renderer->need_resize = true; }

	public:
		BezierCurve() = default;

		inline bool valid() const { return renderer != nullptr; }
		const BezierData& Data() const { return data(); }

		// --- control points ------------------------------------------------------------
		inline DirectX::XMFLOAT3 P0() const { return data().P0; }
		inline DirectX::XMFLOAT3 P1() const { return data().P1; }
		inline DirectX::XMFLOAT3 P2() const { return data().P2; }
		inline DirectX::XMFLOAT3 P3() const { return data().P3; }

		inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1) { data().P0 = P0; data().P1 = P1; data().bezier_power = 1; touch(); }
		inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2) { data().P0 = P0; data().P1 = P1; data().P2 = P2; data().bezier_power = 2; touch(); }
		inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2, const DirectX::XMFLOAT3& P3) { data().P0 = P0; data().P1 = P1; data().P2 = P2; data().P3 = P3; data().bezier_power = 3; touch(); }

		inline int power() { return data().bezier_power; };

		// --- colours -------------------------------------------------------------------
		inline DirectX::XMFLOAT3 C0() const { return data().C0; }
		inline DirectX::XMFLOAT3 C1() const { return data().C1; }
		inline float MinHeight() const { return data().min_height; }
		inline float MaxHeight() const { return data().max_height; }

		inline void colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1) { data().C0 = C0; data().C1 = C1; touch();}
		inline void HeightRange(float min, float max) { data().min_height = min; data().max_height = max; touch(); }

		// --- styles --------------------------------------------------------------------
		inline float Width() const { return data().width; }
		inline CurveCap Cap() const { return data().cap; }
		inline CurveJoin Join() const { return data().join; }
		inline CurvePattern Pattern() const { return data().pattern; }
		inline float Spacing() const { return data().spacing; }

		inline void Width(float value) { data().width = value; touch(); }
		inline void Cap(CurveCap value) { data().cap = value; touch(); }
		inline void Join(CurveJoin value) { data().join = value; touch(); }
		inline void Pattern(CurvePattern value) { data().pattern = value; touch(); }
		inline void Spacing(float value) { data().spacing = value; touch(); }

		// --- resolution ----------------------------------------------------------------
		inline unsigned Resolution() const { return data().resolution; }
		inline void Resolution(unsigned value) { data().resolution = value; touch_layout(); }
	};
}
