#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <span>
#include "pipeline.h"

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

struct UploadBezierData {
	DirectX::XMFLOAT3 P0;
	int32_t  first_index;
	DirectX::XMFLOAT3 P1;
	int32_t  last_index;
	DirectX::XMFLOAT3 P2;
	uint32_t color_begin;
	DirectX::XMFLOAT3 P3;
	uint32_t color_end;
	float    min_height;
	float    max_height;
	float    padding[2];
};

unsigned next_pow2(unsigned x);
uint32_t PackFloat3ToR8G8B8A8(const DirectX::XMFLOAT3& color);
DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);
void ToCubic(const BezierData& source, DirectX::XMFLOAT3& p0, DirectX::XMFLOAT3& p1, DirectX::XMFLOAT3& p2, DirectX::XMFLOAT3& p3);

void ClearComputeBindings(Axodox::Graphics::GraphicsDeviceContext* context);
void ClearDrawBindings(Axodox::Graphics::GraphicsDeviceContext* context);

class BezierCurve;

class BezierRendererBase {
	friend class BezierCurve;
public:
	explicit BezierRendererBase(const Axodox::Graphics::GraphicsDevice& device);
	virtual ~BezierRendererBase() = default;

	BezierRendererBase(const BezierRendererBase&) = delete;
	BezierRendererBase& operator=(const BezierRendererBase&) = delete;

	BezierCurve Add(const BezierData& curve);
	BezierCurve At(size_t index);
	size_t Count() const { return curves.size(); }

	void SetViewport(float width, float height);

	virtual void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) = 0;

protected:
	std::vector<BezierData> curves;

	bool need_resize = true;
	bool need_upload = false;

	uint32_t total_points = 0;

	uint32_t points_allocated = 0;
	uint32_t curves_allocated = 0;

	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data;     // Curve definitions, cubic
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data_map; // One uint32 curve index per point
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_styles;

	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> calculated_points;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> curve_begins;

	Axodox::Graphics::ComputeShader* calc_points = nullptr;

	CameraDataBuffer camera_cb_data{};
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> viewport_data; // Camera / viewport / counts
	Pipeline curve_draw;

	bool UpdateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void UploadCameraData(const DirectX::XMMATRIX& view_proj, Axodox::Graphics::GraphicsDeviceContext* context);

	virtual void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) {}
	virtual void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) {}
	virtual void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) = 0;
	virtual void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) = 0;

private:
	void AllocateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	void UploadCurveData(Axodox::Graphics::GraphicsDeviceContext* context);
};

class BezierCurve {
	friend class BezierRendererBase;
protected:
	size_t curve_index = 0;
	BezierRendererBase* renderer = nullptr;

	BezierCurve(BezierRendererBase* owner, size_t index) : curve_index(index), renderer(owner) {}

	BezierData& data() { return renderer->curves[curve_index]; }
	const BezierData& data() const { return renderer->curves[curve_index]; }

	inline void touch() { renderer->need_upload = true; }
	inline void touch_layout() { renderer->need_resize = true; }

public:
	BezierCurve() = default;

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
};
