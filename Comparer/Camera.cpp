#include "Camera.h"
#include "DirectXMath.h"
using namespace DirectX;

// Reversed-Z perspective
static XMMATRIX ReversedInfinitePerspectiveFovLH(float fovY, float aspect, float zNear)
{
  float sinHalf, cosHalf;
  XMScalarSinCos(&sinHalf, &cosHalf, 0.5f * fovY);
  const float yScale = cosHalf / sinHalf;
  const float xScale = yScale / aspect;
  return XMMATRIX(
    xScale, 0.0f,   0.0f,  0.0f,
    0.0f,   yScale, 0.0f,  0.0f,
    0.0f,   0.0f,   0.0f,  1.0f,
    0.0f,   0.0f,   zNear, 0.0f);
}

Camera::Camera()
{
  SetView(XMVECTOR{ 0.0f, 0.0f, 0.0f, 0.0f }, XMVECTOR{ 0.0f, 0.0f, -1.0f, 0.0f }, XMVECTOR{ 0.0f, 1.0f, 0.0f, 0.0f });
}

CameraData Camera::GetData() const
{
  XMVECTOR determinant;
  return { XMMatrixTranspose(XMMatrixInverse(&determinant, m_viewMatrix)), XMMatrixMultiply(m_viewMatrix, m_projMatrix) };
}

void Camera::SetView(XMVECTOR _eye, XMVECTOR _at, XMVECTOR _worldUp)
{
  m_eye = _eye;
  m_at = _at;
  m_worldUp = _worldUp;

  m_viewMatrix = XMMatrixLookAtLH(m_eye, m_at, m_worldUp);
}

void Camera::SetProj(float _aspect, float _zn)
{
  m_aspect = _aspect;
  m_zNear = _zn;
  m_projMatrix = ReversedInfinitePerspectiveFovLH(m_angle, m_aspect, m_zNear);
}

void Camera::SetAspect(const float _aspect) noexcept
{
  m_aspect = _aspect;
  m_projMatrix = ReversedInfinitePerspectiveFovLH(m_angle, m_aspect, m_zNear);
}

void Camera::SetZNear(const float _zn) noexcept
{
  m_zNear = _zn;
  m_projMatrix = ReversedInfinitePerspectiveFovLH(m_angle, m_aspect, m_zNear);
}
