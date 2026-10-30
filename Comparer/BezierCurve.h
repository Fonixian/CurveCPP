#pragma once
#include <DirectXMath.h>
#include <cassert>
#include "BezierData.h"
#include "BezierHelpers.h"
#include "BezierRendererBase.h"

// Owning, move-only handle to one curve (like a unique_ptr): destroying it removes the curve. The
// renderer keeps a back-pointer (owners) to update curve_index, so handles can be moved freely.
class BezierCurve
{
  friend class BezierRendererBase;

protected:
  size_t curve_index = 0;
  BezierRendererBase* renderer = nullptr;

  BezierCurve(BezierRendererBase* owner, size_t index) :
    curve_index(index),
    renderer(owner)
  {
  }

  DirectX::XMFLOAT3* points() const
  {
    assert(valid());
    return &renderer->cubic_points[curve_index * 4u];
  }

  BezierRendererBase::UploadColorData& color() const
  {
    assert(valid());
    return renderer->color_data[curve_index];
  }

  uint32_t& style() const
  {
    assert(valid());
    return renderer->style_words[curve_index];
  }

  void store_points(const DirectX::XMFLOAT3 (&cubic)[4])
  {
    DirectX::XMFLOAT3* p = points();
    for (int i = 0; i < 4; ++i) p[i] = cubic[i];
    touch(DirtyPositions);
  }

  inline void touch(uint8_t parts = DirtyAll)
  {
    renderer->need_upload = true;
    renderer->upload_parts |= parts;
  }

  inline void touch_layout()
  {
    renderer->need_resize = true;
  }

  inline void kind_changed(CurveKind before)
  {
    if (Kind() != before) renderer->MarkForRouting(curve_index);
  }

public:
  BezierCurve() = default;

  ~BezierCurve()
  {
    Remove();
  }

  BezierCurve(const BezierCurve&) = delete;
  BezierCurve& operator=(const BezierCurve&) = delete;

  BezierCurve(BezierCurve&& other) noexcept;
  BezierCurve& operator=(BezierCurve&& other) noexcept;

  // Splits a merged chain: the next curve starts a new stroke.
  void Remove();

  inline bool valid() const
  {
    return renderer != nullptr;
  }

  inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1)
  {
    DirectX::XMFLOAT3 c[4];
    LinearToCubic(P0, P1, c);
    store_points(c);
  }

  inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2)
  {
    DirectX::XMFLOAT3 c[4];
    QuadraticToCubic(P0, P1, P2, c);
    store_points(c);
  }

  inline void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2, const DirectX::XMFLOAT3& P3)
  {
    const DirectX::XMFLOAT3 c[4] = { P0, P1, P2, P3 };
    store_points(c);
  }

  // Getters return the stored, quantised values (8-bit colour, whole-pixel width).
  inline DirectX::XMFLOAT3 C0() const
  {
    return UnpackFloat3FromR8G8B8A8(color().color_begin);
  }

  inline DirectX::XMFLOAT3 C1() const
  {
    return UnpackFloat3FromR8G8B8A8(color().color_end);
  }

  inline float MinHeight() const
  {
    return color().min_height;
  }

  inline float MaxHeight() const
  {
    return color().max_height;
  }

  inline void colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1)
  {
    color().color_begin = PackFloat3ToR8G8B8A8(C0);
    color().color_end = PackFloat3ToR8G8B8A8(C1);
    touch(DirtyColors);
  }

  inline void HeightRange(float min, float max)
  {
    color().min_height = min;
    color().max_height = max;
    touch(DirtyColors);
  }

  inline float Width() const
  {
    return float(style() >> 24);
  }

  inline CurveCap CapFront() const
  {
    return static_cast<CurveCap>((style() >> 16) & 0xFFu);
  }

  inline CurveCap CapBack() const
  {
    return static_cast<CurveCap>((style() >> 8) & 0xFFu);
  }

  inline CurveJoin Join() const
  {
    return static_cast<CurveJoin>(style() & 0xFFu);
  }

  inline float DashLength() const
  {
    assert(valid());
    return renderer->dash_lengths[curve_index];
  }

  inline float Spacing() const
  {
    assert(valid());
    return renderer->spacings[curve_index];
  }

  inline void Width(float value)
  {
    style() = (style() & 0x00FFFFFFu) | (PackWidthByte(value) << 24);
    touch(DirtyStyles);
  }

  inline void Cap(CurveCap front, CurveCap back)
  {
    style() = (style() & 0xFF0000FFu) | (uint32_t(front) << 16) | (uint32_t(back) << 8);
    touch(DirtyStyles);
  }

  inline void Join(CurveJoin value)
  {
    style() = (style() & ~0xFFu) | uint32_t(value);
    touch(DirtyStyles);
  }

  inline CurveKind Kind() const
  {
    return KindOf(Spacing(), DashLength());
  }

  inline void DashLength(float value)
  {
    const CurveKind kind = Kind();
    renderer->dash_lengths[curve_index] = value;
    touch(DirtyStyles);
    kind_changed(kind);
  }

  inline void Spacing(float value)
  {
    const CurveKind kind = Kind();
    renderer->spacings[curve_index] = value;
    touch(DirtyStyles);
    kind_changed(kind);
  }

  inline void Dot()
  {
    const CurveKind kind = Kind();
    Cap(CurveCap::Round, CurveCap::Round);
    renderer->dash_lengths[curve_index] = 0.f;
    kind_changed(kind);
  }

  inline unsigned Resolution() const
  {
    assert(valid());
    return renderer->resolutions[curve_index];
  }

  inline void Resolution(unsigned value)
  {
    assert(valid());
    renderer->resolutions[curve_index] = value;
    touch_layout();
  }

  // A layout change; setting the current value does nothing.
  inline bool Merged() const
  {
    assert(valid());
    return renderer->merged[curve_index] != 0;
  }

  inline void Merged(bool value)
  {
    if (Merged() != value)
    {
      renderer->merged[curve_index] = value ? 1u : 0u;
      touch_layout();
      renderer->MarkForRouting(curve_index);
    }
  }
};
