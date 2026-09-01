#pragma once
#include <DirectXMath.h>
using namespace DirectX;

struct CameraData
{
	XMMATRIX MIT;
	XMMATRIX VP;
};

class Camera
{
public:
	Camera();

	CameraData GetData() const;

	inline XMVECTOR GetEye() const { return m_eye; }
	inline XMVECTOR GetAt() const { return m_at; }
	inline XMVECTOR GetWorldUp() const { return m_worldUp; }

	inline XMMATRIX GetViewMatrix() const { return m_viewMatrix; }
	inline XMMATRIX GetProj() const { return m_projMatrix; }
	inline XMMATRIX GetViewProj() const { return XMMatrixMultiply(m_viewMatrix, m_projMatrix); }


	void SetView(XMVECTOR _eye, XMVECTOR _at, XMVECTOR _up);
	
	inline float GetAspect() const { return m_aspect; }
	void SetAspect(const float _aspect) noexcept;
	inline float GetZNear() const { return m_zNear; }
	void SetZNear(const float _zn) noexcept;
	inline float GetZFar() const { return m_zFar; }
	void SetZFar(const float _zf) noexcept;

	void SetProj(float _aspect, float _zn, float _zf);

//private:
	XMVECTOR m_eye;
	XMVECTOR m_worldUp;
	XMVECTOR m_at;

	XMMATRIX m_viewMatrix;
	XMMATRIX m_projMatrix;

	float m_zNear = 0.01f;
	float m_zFar = 100.0f;

	float m_angle = DirectX::XMConvertToRadians(45.0f);
	float m_aspect = 1.0f;
};