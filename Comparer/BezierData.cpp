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

BezierData Linear(XMFLOAT3 P0, XMFLOAT3 P1)
{
  BezierData result;
  result.P0 = P0;
  result.P1 = LerpFloat3(P0, P1, 1.0f / 3.0f);
  result.P2 = LerpFloat3(P0, P1, 2.0f / 3.0f);
  result.P3 = P1;
  return result;
}

BezierData Quadratic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2)
{
  BezierData result;
  result.P0 = P0;
  result.P1 = LerpFloat3(P0, P1, 2.0f / 3.0f);
  result.P2 = LerpFloat3(P1, P2, 1.0f / 3.0f);
  result.P3 = P2;
  return result;
}

BezierData Cubic(XMFLOAT3 P0, XMFLOAT3 P1, XMFLOAT3 P2, XMFLOAT3 P3)
{
  BezierData result;
  result.P0 = P0;
  result.P1 = P1;
  result.P2 = P2;
  result.P3 = P3;
  return result;
}
