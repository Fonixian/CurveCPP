#pragma once
#include "BezierData.h"
#include "BezierCurve.h"
#include "CurveRenderer.h"
#include <span>
#include <vector>

// Inner Bezier control points of the segment k1 -> k2. alpha: 0 uniform, 0.5 centripetal, 1 chordal.
void CatmullRomToBezier(const DirectX::XMFLOAT3& k0, const DirectX::XMFLOAT3& k1,
  const DirectX::XMFLOAT3& k2, const DirectX::XMFLOAT3& k3, float alpha,
  DirectX::XMFLOAT3& p1, DirectX::XMFLOAT3& p2);

// size() points -> size() - 1 cubic Bezier curves. The end points only shape the
// end tangents. Changes take effect on Update().
class CatmullRomSpline
{
public:
  CatmullRomSpline() = default;
  // point_count >= 2.
  CatmullRomSpline(CurveRenderer& renderer, const BezierStyle& style, size_t point_count, float alpha = 0.0f);

  CatmullRomSpline(const CatmullRomSpline&) = delete;
  CatmullRomSpline& operator=(const CatmullRomSpline&) = delete;
  CatmullRomSpline(CatmullRomSpline&& other) noexcept;
  CatmullRomSpline& operator=(CatmullRomSpline&& other) noexcept;

  size_t size() const
  {
    return points.size();
  }

  DirectX::XMFLOAT3& operator[](size_t index)
  {
    return points[index];
  }

  const DirectX::XMFLOAT3& operator[](size_t index) const
  {
    return points[index];
  }

  DirectX::XMFLOAT3& FrontEnd()
  {
    return front_end;
  }

  const DirectX::XMFLOAT3& FrontEnd() const
  {
    return front_end;
  }

  DirectX::XMFLOAT3& BackEnd()
  {
    return back_end;
  }

  const DirectX::XMFLOAT3& BackEnd() const
  {
    return back_end;
  }

  // Can be called instead of specifying FrontEnd and BackEnd
  void MirrorEnds();

  float Alpha() const
  {
    return alpha;
  }

  void Alpha(float value);

  void Update();

  // point_count >= 2. Growing re-Adds every link, moving the spline to the end of the draw order.
  void Resize(size_t point_count);

  void Colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1);
  // One per point. With a height range only the first and last are used.
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
  std::vector<DirectX::XMFLOAT3> points;
  DirectX::XMFLOAT3 front_end = { 0.f, 0.f, 0.f };
  DirectX::XMFLOAT3 back_end = { 0.f, 0.f, 0.f };
  float alpha = 0.0f;

  std::vector<DirectX::XMFLOAT3> posed; // knots at the last Update(): front end, points, back end
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
