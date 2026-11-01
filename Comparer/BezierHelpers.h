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

uint32_t PackColor(const DirectX::XMFLOAT4& color);
DirectX::XMFLOAT4 UnpackColor(uint32_t color);
DirectX::XMFLOAT3 LerpFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t);
DirectX::XMFLOAT4 LerpFloat4(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b, float t);

// Top byte of a style word: the HALF width in quarter pixels, so half widths 0..63.75 px in 0.25 steps
// (full widths 0..127.5 px in 0.5 steps). Takes and returns the full width.
constexpr float halfWidthStep = 0.25f;
uint32_t PackWidth(float width);
float UnpackWidth(uint32_t style_word);

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
