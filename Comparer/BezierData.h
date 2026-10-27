#pragma once
#include <DirectXMath.h>

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

uint32_t PackFloat3ToR8G8B8A8(const DirectX::XMFLOAT3& color);
DirectX::XMFLOAT3 UnpackFloat3FromR8G8B8A8(uint32_t color);

struct BezierData {
	// Only stores in cubic form
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
	unsigned resolution = 32u;
	bool merge_with_previous = false;
};

DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);

BezierData Linear(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1);
BezierData Quadratic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2);
BezierData Cubic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2, DirectX::XMFLOAT3 P3);

class BezierRendererBase;

class BezierCurve {
	friend class BezierRendererBase;
protected:
	size_t curve_index = 0;
	BezierRendererBase* renderer = nullptr;

	BezierCurve(BezierRendererBase* owner, size_t index);
	
	uint32_t& style() const;

	void touch(uint8_t parts);
	void touch_layout();

public:
	BezierCurve() = default;
	~BezierCurve();

	BezierCurve(const BezierCurve&) = delete;
	BezierCurve& operator=(const BezierCurve&) = delete;

	BezierCurve(BezierCurve&& other) noexcept;
	BezierCurve& operator=(BezierCurve&& other) noexcept;
	
	void Remove();

	inline bool valid() const { return renderer != nullptr; }

	void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1);
	void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2);
	void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2, const DirectX::XMFLOAT3& P3);

	DirectX::XMFLOAT3 C0() const;
	DirectX::XMFLOAT3 C1() const;
	float MinHeight() const;
	float MaxHeight() const;

	void colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1);
	void HeightRange(float min, float max);

	float Width() const;
	CurveCap CapFront() const;
	CurveCap CapBack() const;
	CurveJoin Join() const;
	float DashLength() const;
	float Spacing() const;

	void Width(float value);
	void Cap(CurveCap front, CurveCap back);
	void Join(CurveJoin value);
	void DashLength(float value);
	void Spacing(float value);

	unsigned Resolution() const;
	void Resolution(unsigned value);
	
	bool Merged() const;
	void Merged(bool value);
};
