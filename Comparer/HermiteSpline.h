#pragma once
#include "BezierData.h"
#include "BezierCurve.h"
#include "CurveRenderer.h"
#include <span>
#include <vector>

struct HermiteKnot
{
  DirectX::XMFLOAT3 position = { 0.f, 0.f, 0.f };
  DirectX::XMFLOAT3 tangent = { 0.f, 0.f, 0.f }; // dP/dt, where t goes 0 -> 1 along each link
};

// Inner Bezier control points of the segment a -> b.
void HermiteToBezier(const HermiteKnot& a, const HermiteKnot& b, DirectX::XMFLOAT3& p1, DirectX::XMFLOAT3& p2);

// size() knots -> size() - 1 cubic links, merged into one stroke. Changes take effect on Update().
class HermiteSpline
{
public:
  HermiteSpline() = default;
  // knot_count >= 2.
  HermiteSpline(CurveRenderer& renderer, const BezierStyle& style, size_t knot_count);

  HermiteSpline(const HermiteSpline&) = delete;
  HermiteSpline& operator=(const HermiteSpline&) = delete;
  HermiteSpline(HermiteSpline&& other) noexcept;
  HermiteSpline& operator=(HermiteSpline&& other) noexcept;

  size_t size() const
  {
    return knots.size();
  }

  HermiteKnot& operator[](size_t index)
  {
    return knots[index];
  }

  const HermiteKnot& operator[](size_t index) const
  {
    return knots[index];
  }

  void Update();

  // knot_count >= 2. Growing re-Adds every link, moving the spline to the end of the draw order.
  void Resize(size_t knot_count);

  void Colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1);
  // One per knot. With a height range only the first and last are used.
  void PointColors(std::span<const DirectX::XMFLOAT3> colors);
  // min < max blends by height over the whole spline, otherwise by t.
  void HeightRange(float min, float max);

  void Width(float value);
  void Cap(CurveCap front, CurveCap back);
  void Join(CurveJoin value);
  void DashLength(float value);
  void Spacing(float value);
  void Resolution(unsigned value);

  unsigned Resolution() const
  {
    return style.resolution;
  }

  const BezierStyle& Style() const
  {
    return style;
  }

  size_t LinkCount() const
  {
    return links.size();
  }

  BezierCurve& Link(size_t index)
  {
    return links[index];
  }

  const BezierCurve& Link(size_t index) const
  {
    return links[index];
  }

  bool valid() const
  {
    return !links.empty() && links.front().valid();
  }

private:
  CurveRenderer* renderer = nullptr;
  std::vector<HermiteKnot> knots;

  std::vector<HermiteKnot> posed; // knots at the last Update()
  bool pose_all = true;

  std::vector<BezierCurve> links;
  BezierStyle style;
  std::vector<DirectX::XMFLOAT3> point_colors; // if empty, uses the color from style along the whole spline

  bool HeightBlend() const
  {
    return style.min_height < style.max_height;
  }

  void AddLinks();
  void ApplyColors();
};
