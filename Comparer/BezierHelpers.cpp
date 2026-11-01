#include "BezierHelpers.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <bit>
#include <limits>

using namespace Axodox::Graphics;
using namespace DirectX;
using namespace DirectX::PackedVector;

uint32_t FitCapacity(uint32_t allocated, uint32_t required)
{
  const uint32_t fitted = required > 0u ? 1u << (std::numeric_limits<uint32_t>::digits - std::countl_zero(required - 1u)) : 1u;
  if (allocated < fitted || fitted <= allocated / 4u) return fitted;
  return allocated;
}

uint32_t PackColor(const XMFLOAT4& color)
{
  XMCOLOR packed;
  XMStoreColor(&packed, XMLoadFloat4(&color));
  return packed.c;
}

XMFLOAT4 UnpackColor(uint32_t color)
{
  const XMCOLOR packed{ color };
  XMFLOAT4 unpacked;
  XMStoreFloat4(&unpacked, XMLoadColor(&packed));
  return unpacked;
}

XMFLOAT3 LerpFloat3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
  XMFLOAT3 out;
  XMStoreFloat3(&out, XMVectorLerp(XMLoadFloat3(&a), XMLoadFloat3(&b), t));
  return out;
}

XMFLOAT4 LerpFloat4(const XMFLOAT4& a, const XMFLOAT4& b, float t)
{
  XMFLOAT4 out;
  XMStoreFloat4(&out, XMVectorLerp(XMLoadFloat4(&a), XMLoadFloat4(&b), t));
  return out;
}

uint32_t PackWidth(float width)
{
  const float halfWidth = std::clamp(width * 0.5f, 0.f, 255.f * halfWidthStep);
  return static_cast<uint32_t>(halfWidth / halfWidthStep + 0.5f) << 24;
}

float UnpackWidth(uint32_t style_word)
{
  return float(style_word >> 24) * halfWidthStep * 2.f;
}

bool EndpointsMeet(const XMFLOAT3X4& prev, const XMFLOAT3X4& next)
{
  const XMVECTOR va = XMVectorSet(prev.m[0][3], prev.m[1][3], prev.m[2][3], 0.f);
  const XMVECTOR vb = XMVectorSet(next.m[0][0], next.m[1][0], next.m[2][0], 0.f);
  const float scale = std::max(1.0f, XMVectorGetX(XMVectorMax(XMVector3Length(va), XMVector3Length(vb))));
  return XMVectorGetX(XMVector3Length(va - vb)) <= 1e-4f * scale;
}

void UnbindShaderResources(GraphicsDeviceContext* context, ShaderStage stage, std::span<const uint32_t> slots)
{
  for (const uint32_t slot : slots) context->BindShaderResourceView(nullptr, stage, slot);
}

void UnbindShaderResources(GraphicsDeviceContext* context, ShaderStage stage, std::initializer_list<uint32_t> slots)
{
  UnbindShaderResources(context, stage, std::span<const uint32_t>(slots.begin(), slots.size()));
}

void UnbindUnorderedAccessViews(GraphicsDeviceContext* context, std::span<const uint32_t> slots)
{
  for (const uint32_t slot : slots) context->BindUnorderedAccessView(nullptr, slot);
}

void UnbindUnorderedAccessViews(GraphicsDeviceContext* context, std::initializer_list<uint32_t> slots)
{
  UnbindUnorderedAccessViews(context, std::span<const uint32_t>(slots.begin(), slots.size()));
}
