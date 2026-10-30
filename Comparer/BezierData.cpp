#include "BezierData.h"
#include "BezierHelpers.h"

using namespace DirectX;

const char* CurveKindName(CurveKind kind)
{
  switch (kind)
  {
  case CurveKind::Solid: return "Solid";
  case CurveKind::Patterned: return "Patterned";
  case CurveKind::Dotted: return "Dots";
  }
  return "?";
}

static BezierData FromCubic(const XMFLOAT3 (&cubic)[4])
{
  BezierData result;
  result.P0 = cubic[0];
  result.P1 = cubic[1];
  result.P2 = cubic[2];
  result.P3 = cubic[3];
  return result;
}

BezierData Linear(XMFLOAT3 P0, XMFLOAT3 P1)
{
  XMFLOAT3 cubic[4];
  LinearToCubic(P0, P1, cubic);
  return FromCubic(cubic);
}

BezierData Quadratic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2)
{
  XMFLOAT3 cubic[4];
  QuadraticToCubic(P0, P1, P2, cubic);
  return FromCubic(cubic);
}

BezierData Cubic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2, XMFLOAT3 P3)
{
  const XMFLOAT3 cubic[4] = { P0, P1, P2, P3 };
  return FromCubic(cubic);
}
