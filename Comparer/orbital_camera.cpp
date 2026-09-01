#include "orbital_camera.h"
#include "Camera.h"
#include <SDL3/SDL.h>
#include <algorithm>

OrbitalCamera::OrbitalCamera()
{
}

OrbitalCamera::~OrbitalCamera()
{
}

void OrbitalCamera::SetCamera(Camera* _pCamera)
{
	m_pCamera = _pCamera;

	if (!m_pCamera) return;

	XMVECTOR ToAim = XMVectorSubtract(m_pCamera->GetAt(), m_pCamera->GetEye());
	XMVECTOR LengthVec = XMVector3Length(ToAim);
	m_distance = XMVectorGetX(LengthVec);

	m_u = atan2f(XMVectorGetZ(ToAim), XMVectorGetX(ToAim));
	m_v = acosf(XMVectorGetY(ToAim) / m_distance);

}

void OrbitalCamera::Update(float _deltaTime)
{
	if (!m_pCamera) return;

	XMVECTOR lookDirection{
		cosf(m_u) * sinf(m_v),
		cosf(m_v),
		-sinf(m_u) * sinf(m_v),
		0.0f
	};
	
	XMVECTOR eye = XMVectorSubtract(m_center, XMVectorScale(lookDirection, m_distance));
	XMVECTOR up = m_pCamera->GetWorldUp();
	XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, lookDirection));
	XMVECTOR forward = XMVector3Cross(right, up);

	
	XMVECTOR direction = XMVectorZero();
	direction = XMVectorMultiplyAdd(forward, XMVectorReplicate(m_goForward), direction);
	direction = XMVectorMultiplyAdd(right, XMVectorReplicate(m_goRight), direction);
	direction = XMVectorMultiplyAdd(up, XMVectorReplicate(m_goUp), direction);
	XMVECTOR deltaPosition = XMVectorScale(direction, m_speed * _deltaTime);

	eye += deltaPosition;
	m_center += deltaPosition;

	m_pCamera->SetView(eye, m_center, m_pCamera->GetWorldUp());
}


void OrbitalCamera::KeyboardDown(const SDL_KeyboardEvent& key)
{
	switch (key.key)
	{
	case SDLK_LSHIFT:
	case SDLK_RSHIFT:
		if (key.repeat == 0) m_speed /= 4.0f;
		break;
	case SDLK_W:
		m_goForward = 1;
		break;
	case SDLK_S:
		m_goForward = -1;
		break;
	case SDLK_A:
		m_goRight = -1;
		break;
	case SDLK_D:
		m_goRight = 1;
		break;
	case SDLK_E:
		m_goUp = 1;
		break;
	case SDLK_Q:
		m_goUp = -1;
		break;
	}
}

void OrbitalCamera::KeyboardUp(const SDL_KeyboardEvent& key)
{

	switch (key.key)
	{
	case SDLK_LSHIFT:
	case SDLK_RSHIFT:
		m_speed *= 4.0f;
		break;
	case SDLK_W:
	case SDLK_S:
		m_goForward = 0;
		break;
	case SDLK_A:
	case SDLK_D:
		m_goRight = 0;
		break;
	case SDLK_Q:
	case SDLK_E:
		m_goUp = 0;
		break;
	}
}


void OrbitalCamera::MouseMove(const SDL_MouseMotionEvent& mouse)
{
	if (mouse.state & SDL_BUTTON_LMASK)
	{
		float du = mouse.xrel / 100.0f;
		float dv = mouse.yrel / 100.0f;

		m_u += du;
		m_v = std::clamp(m_v + dv, 0.1f, 3.1f);
	}
	if (mouse.state & SDL_BUTTON_RMASK)
	{
		float dDistance = mouse.yrel / 100.0f;
		m_distance += dDistance;
	}
}

void OrbitalCamera::MouseWheel(const SDL_MouseWheelEvent& wheel)
{
	float dDistance = static_cast<float>(wheel.y) * m_speed / -100.0f;
	m_distance += dDistance;
}