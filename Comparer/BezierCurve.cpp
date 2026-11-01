#include "BezierCurve.h"
#include <algorithm>
#include <cassert>

using namespace DirectX;

BezierCurve::BezierCurve(BezierRendererBase* owner, size_t index) :
  curve_index(index),
  renderer(owner)
{
}

BezierCurve::~BezierCurve()
{
  Remove();
}

BezierCurve::BezierCurve(BezierCurve&& other) noexcept :
  curve_index(other.curve_index),
  renderer(other.renderer)
{
  if (renderer) renderer->owners[curve_index] = this;
  other.renderer = nullptr;
}

BezierCurve& BezierCurve::operator=(BezierCurve&& other) noexcept
{
  if (this == &other) return *this;

  Remove();

  curve_index = other.curve_index;
  renderer = other.renderer;
  if (renderer) renderer->owners[curve_index] = this;
  other.renderer = nullptr;
  return *this;
}

void BezierCurve::Remove()
{
  if (!renderer) return;

  assert(renderer->owners[curve_index] == this);
  renderer->owners[curve_index] = nullptr;
  ++renderer->removed_count;
  renderer->need_resize = true;
  // The next curve may start a chain of its own now.
  if (curve_index + 1 < renderer->owners.size()) renderer->MarkForRouting(curve_index + 1);
  renderer = nullptr;
}

uint32_t& BezierCurve::style() const
{
  assert(valid());
  return renderer->style_words[curve_index];
}

void BezierCurve::touch(uint8_t parts)
{
  renderer->need_upload = true;
  renderer->upload_parts |= parts;
}

void BezierCurve::touch_layout()
{
  renderer->need_resize = true;
}

void BezierCurve::kind_changed(CurveKind before)
{
  if (Kind() != before) renderer->MarkForRouting(curve_index);
}

void BezierCurve::control_points(const XMFLOAT3& P0, const XMFLOAT3& P1)
{
  assert(valid());
  const XMFLOAT3 A = LerpFloat3(P0, P1, 1.0f / 3.0f);
  const XMFLOAT3 B = LerpFloat3(P0, P1, 2.0f / 3.0f);
  renderer->cubic_points[curve_index] = XMFLOAT3X4{
    P0.x, A.x, B.x, P1.x,
    P0.y, A.y, B.y, P1.y,
    P0.z, A.z, B.z, P1.z
  };
  touch(DirtyPositions);
}

void BezierCurve::control_points(const XMFLOAT3& P0, const XMFLOAT3& P1, const XMFLOAT3& P2)
{
  assert(valid());
  const XMFLOAT3 A = LerpFloat3(P0, P1, 2.0f / 3.0f);
  const XMFLOAT3 B = LerpFloat3(P1, P2, 1.0f / 3.0f);
  renderer->cubic_points[curve_index] = XMFLOAT3X4{
    P0.x, A.x, B.x, P2.x,
    P0.y, A.y, B.y, P2.y,
    P0.z, A.z, B.z, P2.z
  };
  touch(DirtyPositions);
}

void BezierCurve::control_points(const XMFLOAT3& P0, const XMFLOAT3& P1, const XMFLOAT3& P2, const XMFLOAT3& P3)
{
  assert(valid());
  renderer->cubic_points[curve_index] = XMFLOAT3X4{
    P0.x, P1.x, P2.x, P3.x,
    P0.y, P1.y, P2.y, P3.y,
    P0.z, P1.z, P2.z, P3.z
  };
  touch(DirtyPositions);
}

XMFLOAT4 BezierCurve::C0() const
{
  assert(valid());
  return UnpackColor(renderer->color_data[curve_index].color_begin);
}

XMFLOAT4 BezierCurve::C1() const
{
  assert(valid());
  return UnpackColor(renderer->color_data[curve_index].color_end);
}

float BezierCurve::MinHeight() const
{
  assert(valid());
  return renderer->color_data[curve_index].min_height;
}

float BezierCurve::MaxHeight() const
{
  assert(valid());
  return renderer->color_data[curve_index].max_height;
}

void BezierCurve::colors(const XMFLOAT4& C0, const XMFLOAT4& C1)
{
  assert(valid());
  auto& color = renderer->color_data[curve_index];
  color.color_begin = PackColor(C0);
  color.color_end = PackColor(C1);
  touch(DirtyColors);
}

void BezierCurve::HeightRange(float min, float max)
{
  assert(valid());
  auto& color = renderer->color_data[curve_index];
  color.min_height = min;
  color.max_height = max;
  touch(DirtyColors);
}

float BezierCurve::Width() const
{
  return UnpackWidth(style());
}

CurveCap BezierCurve::CapFront() const
{
  return static_cast<CurveCap>((style() >> 16) & 0xFFu);
}

CurveCap BezierCurve::CapBack() const
{
  return static_cast<CurveCap>((style() >> 8) & 0xFFu);
}

CurveJoin BezierCurve::Join() const
{
  return static_cast<CurveJoin>(style() & 0xFFu);
}

float BezierCurve::DashLength() const
{
  assert(valid());
  return renderer->dash_lengths[curve_index];
}

float BezierCurve::Spacing() const
{
  assert(valid());
  return renderer->spacings[curve_index];
}

CurveKind BezierCurve::Kind() const
{
  return KindOf(Spacing(), DashLength());
}

// Full width in px; stored as the half width in quarter pixels (see PackWidth).
void BezierCurve::Width(float value)
{
  style() = (style() & 0x00FFFFFFu) | PackWidth(value);
  touch(DirtyStyles);
}

void BezierCurve::Cap(CurveCap front, CurveCap back)
{
  style() = (style() & 0xFF0000FFu) | (uint32_t(front) << 16) | (uint32_t(back) << 8);
  touch(DirtyStyles);
}

void BezierCurve::Join(CurveJoin value)
{
  style() = (style() & ~0xFFu) | uint32_t(value);
  touch(DirtyStyles);
}

void BezierCurve::DashLength(float value)
{
  const CurveKind kind = Kind();
  renderer->dash_lengths[curve_index] = value;
  touch(DirtyStyles);
  kind_changed(kind);
}

void BezierCurve::Spacing(float value)
{
  const CurveKind kind = Kind();
  renderer->spacings[curve_index] = value;
  touch(DirtyStyles);
  kind_changed(kind);
}

unsigned BezierCurve::Resolution() const
{
  assert(valid());
  return renderer->resolutions[curve_index];
}

void BezierCurve::Resolution(unsigned value)
{
  assert(valid());
  renderer->resolutions[curve_index] = value;
  touch_layout();
}

bool BezierCurve::Merged() const
{
  assert(valid());
  return renderer->merged[curve_index] != 0;
}

void BezierCurve::Merged(bool value)
{
  if (Merged() != value)
  {
    renderer->merged[curve_index] = value ? 1u : 0u;
    touch_layout();
    renderer->MarkForRouting(curve_index);
  }
}
