#include "BezierHelpers.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

using namespace Axodox::Graphics;
using namespace DirectX;

unsigned next_pow2(unsigned x)
{
  return x <= 1u ? 1u : 1u << (std::numeric_limits<unsigned>::digits - std::countl_zero(x - 1u));
}

uint32_t FitCapacity(uint32_t allocated, uint32_t required)
{
  const uint32_t fitted = next_pow2(std::max(required, 1u));
  if (allocated < fitted) return fitted;
  if (fitted <= allocated / 4u) return fitted;
  return allocated;
}

uint32_t PackFloat3ToR8G8B8A8(const XMFLOAT3& color)
{
  const float r = std::clamp(color.x, 0.0f, 1.0f);
  const float g = std::clamp(color.y, 0.0f, 1.0f);
  const float b = std::clamp(color.z, 0.0f, 1.0f);

  const uint32_t ir = static_cast<uint32_t>(r * 255.0f + 0.5f);
  const uint32_t ig = static_cast<uint32_t>(g * 255.0f + 0.5f);
  const uint32_t ib = static_cast<uint32_t>(b * 255.0f + 0.5f);
  const uint32_t ia = 255u;

  return (ia << 24) | (ib << 16) | (ig << 8) | ir;
}

XMFLOAT3 UnpackFloat3FromR8G8B8A8(uint32_t color)
{
  return XMFLOAT3{
    float(color & 0xFFu) / 255.0f,
    float((color >> 8) & 0xFFu) / 255.0f,
    float((color >> 16) & 0xFFu) / 255.0f
  };
}

uint32_t PackWidthByte(float width)
{
  return static_cast<uint32_t>(std::clamp(width, 0.f, 255.f) + 0.5f);
}

uint32_t PackWidthCapCapJoin(const BezierData& bez)
{
  return (PackWidthByte(bez.width) << 24) |
    (uint32_t(bez.cap_front) << 16) |
    (uint32_t(bez.cap_back) << 8) |
    uint32_t(bez.join);
}

XMFLOAT3 LerpFloat3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
  XMFLOAT3 out;
  XMStoreFloat3(&out, XMVectorLerp(XMLoadFloat3(&a), XMLoadFloat3(&b), t));
  return out;
}

void LinearToCubic(const XMFLOAT3& p0, const XMFLOAT3& p1, XMFLOAT3 (&cubic)[4])
{
  cubic[0] = p0;
  cubic[1] = LerpFloat3(p0, p1, 1.0f / 3.0f);
  cubic[2] = LerpFloat3(p0, p1, 2.0f / 3.0f);
  cubic[3] = p1;
}

void QuadraticToCubic(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, XMFLOAT3 (&cubic)[4])
{
  cubic[0] = p0;
  cubic[1] = LerpFloat3(p0, p1, 2.0f / 3.0f);
  cubic[2] = LerpFloat3(p1, p2, 1.0f / 3.0f);
  cubic[3] = p2;
}

constexpr uint32_t computeSrvSlots = 5u;
constexpr uint32_t computeUavSlots = 5u;

void ClearComputeBindings(GraphicsDeviceContext* context)
{
  for (uint32_t slot = 0; slot < computeSrvSlots; ++slot)
  {
    context->BindShaderResourceView(nullptr, ShaderStage::Compute, slot);
  }
  for (uint32_t slot = 0; slot < computeUavSlots; ++slot)
  {
    context->BindUnorderedAccessView(nullptr, slot);
  }
}

// The patterned renderer's CurveVs binds t0..t8.
constexpr uint32_t vertexSrvSlots = 9u;
// Only t1 (pattern positions) is ever bound to the pixel stage.
constexpr uint32_t pixelSrvSlots = 2u;

void ClearDrawBindings(GraphicsDeviceContext* context)
{
  for (uint32_t slot = 0; slot < vertexSrvSlots; ++slot)
  {
    context->BindShaderResourceView(nullptr, ShaderStage::Vertex, slot);
  }
  for (uint32_t slot = 1; slot < pixelSrvSlots; ++slot)
  {
    context->BindShaderResourceView(nullptr, ShaderStage::Pixel, slot);
  }
}

uint64_t PatternCenterBound(const XMFLOAT3& p0, const XMFLOAT3& p1, const XMFLOAT3& p2, const XMFLOAT3& p3, float spacing)
{
  if (!(spacing > 0.f)) return 0;

  const float polygon =
    XMVectorGetX(XMVector3Length(XMLoadFloat3(&p1) - XMLoadFloat3(&p0))) +
    XMVectorGetX(XMVector3Length(XMLoadFloat3(&p2) - XMLoadFloat3(&p1))) +
    XMVectorGetX(XMVector3Length(XMLoadFloat3(&p3) - XMLoadFloat3(&p2)));

  const double centers = std::floor(static_cast<double>(polygon) / static_cast<double>(spacing)) + 1.0;
  return static_cast<uint64_t>(std::clamp(centers, 0.0, static_cast<double>(maxPatternCount)));
}

bool EndpointsMeet(const XMFLOAT3& a, const XMFLOAT3& b)
{
  const XMVECTOR va = XMLoadFloat3(&a);
  const XMVECTOR vb = XMLoadFloat3(&b);
  const float scale = std::max(1.0f, XMVectorGetX(XMVectorMax(XMVector3Length(va), XMVector3Length(vb))));
  return XMVectorGetX(XMVector3Length(va - vb)) <= 1e-4f * scale;
}
