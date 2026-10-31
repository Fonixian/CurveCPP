#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include <span>
#include <initializer_list>
#include "BezierData.h"

// Cap on the pattern bound, so a near-zero spacing cannot ask for billions of slots.
constexpr uint32_t maxPatternCount = 4'000'000u;

// Capacity of every scan instance (points, and therefore curves too).
constexpr uint32_t maxElementCount = 1'200'000u;

// Grows to the next power of two of `required` when it no longer fits, shrinks to it once usage drops
// to a quarter, otherwise keeps `allocated`.
uint32_t FitCapacity(uint32_t allocated, uint32_t required);

constexpr uint32_t GroupCount(uint32_t items, uint32_t group_size)
{
  return (items + group_size - 1u) / group_size;
}

// DirectX::XMCOLOR packing: a << 24 | r << 16 | g << 8 | b, alpha 255.
uint32_t PackFloat3ToR8G8B8A8(const DirectX::XMFLOAT3& color);
DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);
DirectX::XMFLOAT3 UnpackFloat3FromR8G8B8A8(uint32_t color);

// Whether `prev` ends where `next` starts: P3 of `prev` (its last column) against P0 of `next` (its
// first column). Relative tolerance; used by the merge asserts.
bool EndpointsMeet(const DirectX::XMFLOAT3X4& prev, const DirectX::XMFLOAT3X4& next);

// Bind nullptr to each listed slot: SRVs of `stage`, or compute UAVs. Every pass unbinds exactly the
// slots it bound once it is done, so the next pass starts from clean slots and a buffer can switch
// between SRV and UAV.
void UnbindShaderResources(Axodox::Graphics::GraphicsDeviceContext* context, Axodox::Graphics::ShaderStage stage, std::span<const uint32_t> slots);
void UnbindShaderResources(Axodox::Graphics::GraphicsDeviceContext* context, Axodox::Graphics::ShaderStage stage, std::initializer_list<uint32_t> slots);
void UnbindUnorderedAccessViews(Axodox::Graphics::GraphicsDeviceContext* context, std::span<const uint32_t> slots);
void UnbindUnorderedAccessViews(Axodox::Graphics::GraphicsDeviceContext* context, std::initializer_list<uint32_t> slots);
