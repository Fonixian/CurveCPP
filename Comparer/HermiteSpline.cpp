#include "HermiteSpline.h"
#include <cassert>
#include <cstring>
#include <utility>

using namespace DirectX;

void HermiteToBezier(const HermiteKnot& a, const HermiteKnot& b, XMFLOAT3& p1, XMFLOAT3& p2)
{
  constexpr float third = 1.0f / 3.0f;
  p1 = {
    a.position.x + a.tangent.x * third,
    a.position.y + a.tangent.y * third,
    a.position.z + a.tangent.z * third
  };
  p2 = {
    b.position.x - b.tangent.x * third,
    b.position.y - b.tangent.y * third,
    b.position.z - b.tangent.z * third
  };
}

HermiteSpline::HermiteSpline(CurveRenderer& target, const BezierStyle& link_style, size_t knot_count) :
  renderer(&target),
  knots(knot_count),
  style(link_style)
{
  assert(knot_count >= 2 && "a Hermite spline needs at least 2 knots");
  AddLinks();
}

HermiteSpline::HermiteSpline(HermiteSpline&& other) noexcept :
  renderer(std::exchange(other.renderer, nullptr)),
  knots(std::move(other.knots)),
  posed(std::move(other.posed)),
  pose_all(other.pose_all),
  links(std::move(other.links)),
  style(other.style),
  point_colors(std::move(other.point_colors))
{
}

HermiteSpline& HermiteSpline::operator=(HermiteSpline&& other) noexcept
{
  if (this == &other) return *this;
  links = std::move(other.links);
  renderer = std::exchange(other.renderer, nullptr);
  knots = std::move(other.knots);
  posed = std::move(other.posed);
  pose_all = other.pose_all;
  style = other.style;
  point_colors = std::move(other.point_colors);
  return *this;
}

void HermiteSpline::Update()
{
  const size_t n = knots.size();
  if (n < 2 || links.empty()) return;

  const bool all = pose_all || posed.size() != n;
  if (all) posed.resize(n);

  std::vector<bool> changed(n, all);
  if (!all)
  {
    for (size_t j = 0; j < n; ++j) changed[j] = std::memcmp(&knots[j], &posed[j], sizeof(HermiteKnot)) != 0;
  }

  for (size_t i = 0; i < links.size(); ++i)
  {
    if (!(changed[i] || changed[i + 1])) continue;

    XMFLOAT3 p1;
    XMFLOAT3 p2;
    HermiteToBezier(knots[i], knots[i + 1], p1, p2);
    links[i].control_points(knots[i].position, p1, p2, knots[i + 1].position);
  }

  posed = knots;
  pose_all = false;
}

void HermiteSpline::Resize(size_t knot_count)
{
  assert(knot_count >= 2 && "a Hermite spline needs at least 2 knots");
  assert(renderer && "Resize() on a default-constructed spline");
  if (knot_count == knots.size()) return;

  const HermiteKnot last = knots.back();
  knots.resize(knot_count, last);
  point_colors.clear();
  pose_all = true;

  const size_t link_count = knot_count - 1;
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

void HermiteSpline::AddLinks()
{
  const size_t count = knots.size() - 1;
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
void HermiteSpline::ApplyColors()
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

void HermiteSpline::Colors(const XMFLOAT3& C0, const XMFLOAT3& C1)
{
  style.C0 = C0;
  style.C1 = C1;
  point_colors.clear();
  ApplyColors();
}

void HermiteSpline::PointColors(std::span<const XMFLOAT3> colors)
{
  assert(colors.size() == knots.size() && "one colour per knot");
  point_colors.assign(colors.begin(), colors.end());
  ApplyColors();
}

void HermiteSpline::HeightRange(float min, float max)
{
  if (min == style.min_height && max == style.max_height) return;

  const bool was_height = HeightBlend();
  style.min_height = min;
  style.max_height = max;
  for (auto& link : links) link.HeightRange(min, max);
  if (HeightBlend() != was_height) ApplyColors();
}

void HermiteSpline::Width(float value)
{
  style.width = value;
  for (auto& link : links) link.Width(value);
}

void HermiteSpline::Cap(CurveCap front, CurveCap back)
{
  style.cap_front = front;
  style.cap_back = back;
  for (auto& link : links) link.Cap(front, back);
}

void HermiteSpline::Join(CurveJoin value)
{
  style.join = value;
  for (auto& link : links) link.Join(value);
}

void HermiteSpline::DashLength(float value)
{
  style.dash_length = value;
  for (auto& link : links) link.DashLength(value);
}

void HermiteSpline::Spacing(float value)
{
  style.spacing = value;
  for (auto& link : links) link.Spacing(value);
}

void HermiteSpline::Resolution(unsigned value)
{
  style.resolution = value;
  for (auto& link : links)
  {
    if (link.Resolution() != value) link.Resolution(value);
  }
}
