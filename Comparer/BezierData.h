#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>

enum class CurveCap : uint8_t
{
  Butt = 0,
  Square = 1,
  Round = 2,
  TriangleOut = 3,
  TriangleIn = 4
};

enum class CurveJoin : uint8_t
{
  Round = 0,
  Square = 1
};

struct BezierData
{
  // Always cubic: Linear() and Quadratic() raise to cubic.
  DirectX::XMFLOAT3 P0 = { 0.f, 0.f, 0.f };
  DirectX::XMFLOAT3 P1 = { 0.f, 0.f, 0.f };
  DirectX::XMFLOAT3 P2 = { 0.f, 0.f, 0.f };
  DirectX::XMFLOAT3 P3 = { 0.f, 0.f, 0.f };
  DirectX::XMFLOAT3 C0 = { 1.f, 1.f, 1.f };
  DirectX::XMFLOAT3 C1 = { 1.f, 1.f, 1.f };
  float min_height = 0.f;
  float max_height = 0.f;
  float width = 2.f;
  CurveCap cap_front = CurveCap::Butt;
  CurveCap cap_back = CurveCap::Butt;
  CurveJoin join = CurveJoin::Round;
  float dash_length = 0.f;
  float spacing = 0.f;
  unsigned resolution = 64u;
  // Continue the stroke of the curve Added just before this one (one arc length, one pattern, caps
  // only at the chain's two ends). Requires P0 == the previous P3; the joint sample is shared, so a
  // merged curve adds resolution - 1 samples. Ignored on curve 0. In a BezierRenderer the whole
  // chain is routed by its first curve.
  bool merge_with_previous = false;
};

BezierData Linear(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1);
BezierData Quadratic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2);
BezierData Cubic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2, DirectX::XMFLOAT3 P3);

// Which backend of a BezierRenderer draws a curve.
enum class CurveKind : uint8_t
{
  Solid = 0,     // BezierSolidRenderer
  Patterned = 1, // BezierPatternRenderer
  Dotted = 2     // BezierDotRenderer
};
constexpr size_t CurveKindCount = 3;
const char* CurveKindName(CurveKind kind);

// spacing 0 -> Solid, dash length 0 -> Dotted, otherwise Patterned ("0" = not above zero, NaN too).
constexpr CurveKind KindOf(float spacing, float dash_length)
{
  return !(spacing > 0.f)  ? CurveKind::Solid
    : !(dash_length > 0.f) ? CurveKind::Dotted
                           : CurveKind::Patterned;
}

constexpr CurveKind KindOf(const BezierData& curve)
{
  return KindOf(curve.spacing, curve.dash_length);
}
