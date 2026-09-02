#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include <vector>
#include <span>
#include <memory>
#include <limits>
#include "SegmentedScan.h"
#include "pipeline.h"

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
		Round = 0,
		Square = 1
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
		// Styles
		float width = 2.f;
		CurveCap cap = CurveCap::Butt;
		CurveJoin join = CurveJoin::Round;
		CurvePattern pattern = CurvePattern::Solid;
		float spacing = 1.f;
		unsigned resolution = 64u;
		int bezier_power = 1;

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

	class BezierCurve;

	class BezierRenderer {
		friend class BezierCurve;
	public:
		explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

		BezierCurve Add(const BezierData& curve);

		BezierCurve At(size_t index);
		size_t Count() const { return curves.size(); }

		void SetViewport(float width, float height);

		void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj);

		static constexpr float DashLengthPerWidth = 4.0f;
		static constexpr float DotLengthPerWidth = 1.0f;

	protected:
		std::vector<BezierData> curves;

		void AllocateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
		void UploadCurveData(Axodox::Graphics::GraphicsDeviceContext* context);
		void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
		void CountPatternCenters(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
		void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

		// --- dirty flags ---------------------------------------------------------------
		bool need_resize = true;
		bool need_upload = false;
		bool need_recount = false;

		uint32_t total_points = 0;
		uint32_t pattern_total = 0;

		uint32_t points_allocated = 0;
		uint32_t curves_allocated = 0;
		uint32_t patterns_allocated = 0;

		// --- layout-time inputs --------------------------------------------------------
		std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data;     // Curve definitions, cubic
		std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_styles;    // Width / cap / join / pattern / spacing
		std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data_map; // One uint32 curve index per point

		// --- per-frame compute results -------------------------------------------------
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> calculated_points; // Point positions and packed colours
		std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;
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
