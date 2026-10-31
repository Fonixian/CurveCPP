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

  BezierCurve(BezierRendererBase* owner, size_t index);

  uint32_t& style() const;

  void touch(uint8_t parts);
  void touch_layout();
  void kind_changed(CurveKind before);

public:
  BezierCurve() = default;
  ~BezierCurve();

  BezierCurve(const BezierCurve&) = delete;
  BezierCurve& operator=(const BezierCurve&) = delete;

  BezierCurve(BezierCurve&& other) noexcept;
  BezierCurve& operator=(BezierCurve&& other) noexcept;

  // Splits a merged chain: the next curve starts a new stroke.
  void Remove();

  bool valid() const
  {
    return renderer != nullptr;
  }

  // Lines and quadratics are raised to cubic on the way in.
  void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1);
  void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2);
  void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2, const DirectX::XMFLOAT3& P3);

  // Getters return the stored, quantised values (8-bit colour, whole-pixel width).
  DirectX::XMFLOAT3 C0() const;
  DirectX::XMFLOAT3 C1() const;
  float MinHeight() const;
  float MaxHeight() const;

  void colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1);
  void HeightRange(float min, float max);

  float Width() const;
  CurveCap CapFront() const;
  CurveCap CapBack() const;
  CurveJoin Join() const;
  float DashLength() const;
  float Spacing() const;
  CurveKind Kind() const;

  void Width(float value);
  void Cap(CurveCap front, CurveCap back);
  void Join(CurveJoin value);
  void DashLength(float value);
  void Spacing(float value);

  unsigned Resolution() const;
  void Resolution(unsigned value);

  // A layout change; setting the current value does nothing.
  bool Merged() const;
  void Merged(bool value);
};
