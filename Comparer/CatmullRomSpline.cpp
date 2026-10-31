#include "CatmullRomSpline.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <utility>

using namespace DirectX;

// Non-uniform tangents (Barry-Goldman), with knot intervals d_i = |k_{i+1} - k_i|^alpha:
//   m1 = (k1 - k0)/d0 - (k2 - k0)/(d0 + d1) + (k2 - k1)/d1
//   m2 = (k2 - k1)/d1 - (k3 - k1)/(d1 + d2) + (k3 - k2)/d2
// Handles: k1 + m1 * d1/3, k2 - m2 * d1/3.
void CatmullRomToBezier(const XMFLOAT3& k0, const XMFLOAT3& k1, const XMFLOAT3& k2, const XMFLOAT3& k3,
  float alpha, XMFLOAT3& p1, XMFLOAT3& p2)
{
  const XMVECTOR v0 = XMLoadFloat3(&k0);
  const XMVECTOR v1 = XMLoadFloat3(&k1);
  const XMVECTOR v2 = XMLoadFloat3(&k2);
  const XMVECTOR v3 = XMLoadFloat3(&k3);

  if (alpha == 0.0f)
  {
    XMStoreFloat3(&p1, v1 + (v2 - v0) * (1.0f / 6.0f));
    XMStoreFloat3(&p2, v2 - (v3 - v1) * (1.0f / 6.0f));
    return;
  }

  // Avoids 0/0 on coincident knots.
  constexpr float MinKnotInterval = 1e-4f;
  const float d0 = std::max(std::pow(XMVectorGetX(XMVector3Length(v1 - v0)), alpha), MinKnotInterval);
  const float d1 = std::max(std::pow(XMVectorGetX(XMVector3Length(v2 - v1)), alpha), MinKnotInterval);
  const float d2 = std::max(std::pow(XMVectorGetX(XMVector3Length(v3 - v2)), alpha), MinKnotInterval);

  const XMVECTOR m1 = (v1 - v0) / d0 - (v2 - v0) / (d0 + d1) + (v2 - v1) / d1;
  const XMVECTOR m2 = (v2 - v1) / d1 - (v3 - v1) / (d1 + d2) + (v3 - v2) / d2;

  const float handle = d1 / 3.0f;
  XMStoreFloat3(&p1, v1 + m1 * handle);
  XMStoreFloat3(&p2, v2 - m2 * handle);
}

CatmullRomSpline::CatmullRomSpline(CurveRenderer& target, const BezierStyle& link_style, size_t point_count, float link_alpha) :
  renderer(&target),
  points(point_count, XMFLOAT3{ 0.f, 0.f, 0.f }),
  alpha(link_alpha),
  style(link_style)
{
  assert(point_count >= 2 && "a Catmull-Rom spline needs at least 2 points");
  AddLinks();
}

CatmullRomSpline::CatmullRomSpline(CatmullRomSpline&& other) noexcept :
  renderer(std::exchange(other.renderer, nullptr)),
  points(std::move(other.points)),
  front_end(other.front_end),
  back_end(other.back_end),
  alpha(other.alpha),
  posed(std::move(other.posed)),
  pose_all(other.pose_all),
  links(std::move(other.links)),
  style(other.style),
  point_colors(std::move(other.point_colors))
{
}

CatmullRomSpline& CatmullRomSpline::operator=(CatmullRomSpline&& other) noexcept
{
  if (this == &other) return *this;
  links = std::move(other.links);
  renderer = std::exchange(other.renderer, nullptr);
  points = std::move(other.points);
  front_end = other.front_end;
  back_end = other.back_end;
  alpha = other.alpha;
  posed = std::move(other.posed);
  pose_all = other.pose_all;
  style = other.style;
  point_colors = std::move(other.point_colors);
  return *this;
}

void CatmullRomSpline::MirrorEnds()
{
  const size_t n = points.size();
  const XMFLOAT3& p0 = points[0];
  const XMFLOAT3& p1 = points[1];
  const XMFLOAT3& q0 = points[n - 1];
  const XMFLOAT3& q1 = points[n - 2];
  front_end = { 2.0f * p0.x - p1.x, 2.0f * p0.y - p1.y, 2.0f * p0.z - p1.z };
  back_end = { 2.0f * q0.x - q1.x, 2.0f * q0.y - q1.y, 2.0f * q0.z - q1.z };
}

void CatmullRomSpline::Alpha(float value)
{
  if (value == alpha) return;
  alpha = value;
  pose_all = true;
}

void CatmullRomSpline::Update()
{
  const size_t n = points.size();
  if (n < 2 || links.empty()) return;

  // 0: front end, 1..n: points, n + 1: back end. Link i uses knots i..i + 3.
  auto knot = [&](size_t j) -> const XMFLOAT3&
  {
    if (j == 0) return front_end;
    if (j == n + 1) return back_end;
    return points[j - 1];
  };

  const bool all = pose_all || posed.size() != n + 2;
  if (all) posed.resize(n + 2);

  std::vector<bool> changed(n + 2, all);
  if (!all)
  {
    for (size_t j = 0; j < n + 2; ++j) changed[j] = std::memcmp(&knot(j), &posed[j], sizeof(XMFLOAT3)) != 0;
  }

  for (size_t i = 0; i < links.size(); ++i)
  {
    if (!(changed[i] || changed[i + 1] || changed[i + 2] || changed[i + 3])) continue;

    const XMFLOAT3& k0 = knot(i);
    const XMFLOAT3& k1 = knot(i + 1);
    const XMFLOAT3& k2 = knot(i + 2);
    const XMFLOAT3& k3 = knot(i + 3);

    XMFLOAT3 p1;
    XMFLOAT3 p2;
    CatmullRomToBezier(k0, k1, k2, k3, alpha, p1, p2);
    links[i].control_points(k1, p1, p2, k2);
  }

  for (size_t j = 0; j < n + 2; ++j) posed[j] = knot(j);
  pose_all = false;
}

void CatmullRomSpline::Resize(size_t point_count)
{
  assert(point_count >= 2 && "a Catmull-Rom spline needs at least 2 points");
  assert(renderer && "Resize() on a default-constructed spline");
  if (point_count == points.size()) return;

  const XMFLOAT3 last = points.back();
  points.resize(point_count, last);
  point_colors.clear();
  pose_all = true;

  const size_t link_count = point_count - 1;
  if (link_count < links.size())
  {
    links.erase(links.begin() + std::ptrdiff_t(link_count), links.end());
    ApplyColors();
  }
  else
  {
    links.clear();
    AddLinks();
  }
}

void CatmullRomSpline::AddLinks()
{
  const size_t count = points.size() - 1;
  links.reserve(count);
  for (size_t i = 0; i < count; ++i)
  {
    BezierData link;
    link.style = style;
    link.merge_with_previous = i > 0;
    links.push_back(renderer->Add(link));
  }
  ApplyColors();
  pose_all = true;
}

// By height every link gets the spline's end colours; by t each link gets its slice.
void CatmullRomSpline::ApplyColors()
{
  const size_t count = links.size();
  if (count == 0) return;

  const bool by_point = !point_colors.empty();
  if (HeightBlend())
  {
    const XMFLOAT3 C0 = by_point ? point_colors.front() : style.C0;
    const XMFLOAT3 C1 = by_point ? point_colors.back() : style.C1;
    for (auto& link : links) link.colors(C0, C1);
  }
  else if (by_point)
  {
    for (size_t i = 0; i < count; ++i) links[i].colors(point_colors[i], point_colors[i + 1]);
  }
  else
  {
    for (size_t i = 0; i < count; ++i)
    {
      links[i].colors(
        LerpFloat3(style.C0, style.C1, float(i) / float(count)),
        LerpFloat3(style.C0, style.C1, float(i + 1) / float(count)));
    }
  }
}

void CatmullRomSpline::Colors(const XMFLOAT3& C0, const XMFLOAT3& C1)
{
  style.C0 = C0;
  style.C1 = C1;
  point_colors.clear();
  ApplyColors();
}

void CatmullRomSpline::PointColors(std::span<const XMFLOAT3> colors)
{
  assert(colors.size() == points.size() && "one colour per point");
  point_colors.assign(colors.begin(), colors.end());
  ApplyColors();
}

void CatmullRomSpline::HeightRange(float min, float max)
{
  if (min == style.min_height && max == style.max_height) return;

  const bool was_height = HeightBlend();
  style.min_height = min;
  style.max_height = max;
  for (auto& link : links) link.HeightRange(min, max);
  if (HeightBlend() != was_height) ApplyColors();
}

void CatmullRomSpline::Width(float value)
{
  style.width = value;
  for (auto& link : links) link.Width(value);
}

void CatmullRomSpline::Cap(CurveCap front, CurveCap back)
{
  style.cap_front = front;
  style.cap_back = back;
  for (auto& link : links) link.Cap(front, back);
}

void CatmullRomSpline::Join(CurveJoin value)
{
  style.join = value;
  for (auto& link : links) link.Join(value);
}

void CatmullRomSpline::DashLength(float value)
{
  style.dash_length = value;
  for (auto& link : links) link.DashLength(value);
}

void CatmullRomSpline::Spacing(float value)
{
  style.spacing = value;
  for (auto& link : links) link.Spacing(value);
}

void CatmullRomSpline::Resolution(unsigned value)
{
  style.resolution = value;
  for (auto& link : links)
  {
    if (link.Resolution() != value) link.Resolution(value);
  }
}
