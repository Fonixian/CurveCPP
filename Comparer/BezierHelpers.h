#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include "BezierData.h"

// Cap on the pattern bound, so a near-zero spacing cannot ask for billions of slots.
constexpr uint32_t maxPatternCount = 4'000'000u;

// Capacity of every scan instance (points, and therefore curves too).
constexpr uint32_t maxElementCount = 1'200'000u;

// Grows to next_pow2(required) when it no longer fits, shrinks to it once usage drops to a quarter,
// otherwise keeps `allocated`.
uint32_t FitCapacity(uint32_t allocated, uint32_t required);

constexpr uint32_t GroupCount(uint32_t items, uint32_t group_size)
{
  return (items + group_size - 1u) / group_size;
}

uint32_t PackFloat3ToR8G8B8A8(const DirectX::XMFLOAT3& color);
DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);
DirectX::XMFLOAT3 UnpackFloat3FromR8G8B8A8(uint32_t color);

void LinearToCubic(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, DirectX::XMFLOAT3 (&cubic)[4]);
void QuadraticToCubic(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2, DirectX::XMFLOAT3 (&cubic)[4]);

// Rounded to a whole pixel, clamped to [0, 255].
uint32_t PackWidthByte(float width);

// width << 24 | cap_front << 16 | cap_back << 8 | join
uint32_t PackWidthCapCapJoin(const BezierData& bez);

// floor(control polygon / spacing) + 1, or 0 when spacing <= 0.
uint64_t PatternCenterBound(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, const DirectX::XMFLOAT3& p2, const DirectX::XMFLOAT3& p3, float spacing);

// Relative tolerance; used by the merge asserts.
bool EndpointsMeet(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b);

void ClearComputeBindings(Axodox::Graphics::GraphicsDeviceContext* context);
void ClearDrawBindings(Axodox::Graphics::GraphicsDeviceContext* context);
