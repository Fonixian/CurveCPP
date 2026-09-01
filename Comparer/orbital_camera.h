#pragma once
#include "camera.h"
#include <DirectXMath.h>
using namespace DirectX;

class Camera;

struct SDL_KeyboardEvent;
struct SDL_MouseMotionEvent;
struct SDL_MouseWheelEvent;

class OrbitalCamera
{
public:
	OrbitalCamera();

	~OrbitalCamera();

	void SetCamera(Camera* _pCamera);
	void Update(float _deltaTime);

	inline void  SetSpeed(float _speed) { m_speed = _speed; }
	inline float GetSpeed() const noexcept { return m_speed; }

	void KeyboardDown(const SDL_KeyboardEvent& key);
	void KeyboardUp(const SDL_KeyboardEvent& key);
	void MouseMove(const SDL_MouseMotionEvent& mouse);
	void MouseWheel(const SDL_MouseWheelEvent& wheel);

private:
	Camera* m_pCamera = nullptr;

	float m_u = 0.0f;
	float m_v = 0.0f;

	float m_distance = 0.0f;

	XMVECTOR m_center{ 0.0f, 0.0f, 0.0f, 0.0f };

	float m_speed = 16.0f;

	float	m_goForward = 0.0f;
	float	m_goRight = 0.0f;
	float   m_goUp = 0.0f;
};