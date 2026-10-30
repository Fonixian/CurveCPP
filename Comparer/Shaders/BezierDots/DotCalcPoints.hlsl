#include "DotCommon.hlsli"

// World segment lengths only.

cbuffer CameraData : register(b0)
{
  float4x4 VP;
  float2 WH;
  uint TotalPointCount;
  uint TotalCurveCount;
};

StructuredBuffer<float3> ControlPoints : register(t0);
StructuredBuffer<uint> BezierIndexMap : register(t1);
StructuredBuffer<uint2> CurveIndices : register(t2);

RWStructuredBuffer<float> WorldDistances : register(u0);

Cubic LoadCubic(uint curveIndex)
{
  uint k = curveIndex << 2u;
  Cubic cubic;
  cubic.P0 = ControlPoints[k];
  cubic.P1 = ControlPoints[k + 1u];
  cubic.P2 = ControlPoints[k + 2u];
  cubic.P3 = ControlPoints[k + 3u];
  return cubic;
}

// One thread measures TWO consecutive points: i0 = 2 * id and i1 = i0 + 1. Point i's value is the
// chord from sample i to sample i + 1 (the next index clamped to the curve's range), so the pair needs
// the positions at i0, i0 + 1 and i0 + 2. When i1 sits on the same curve as i0 the middle one is
// shared - 3 curve evaluations instead of 4, and one curve lookup instead of two. Dispatch
// GroupCount(TotalPointCount, 512).
[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
  uint i0 = dispatchId.x << 1u;
  if (i0 >= TotalPointCount) return;
  uint i1 = i0 + 1u;

  uint curve0 = BezierIndexMap[i0];
  uint2 range0 = CurveIndices[curve0];
  Cubic cubic0 = LoadCubic(curve0);

  float3 pos0 = EvaluateBezier(cubic0, SampleT(range0, i0));
  float3 pos1 = EvaluateBezier(cubic0, SampleT(range0, min(i1, range0.y)));
  WorldDistances[i0] = distance(pos0, pos1);

  if (i1 >= TotalPointCount) return;

  uint curve1 = BezierIndexMap[i1];
  if (curve1 == curve0 && i1 <= range0.y)
  {
    // Same curve and i1 is not clamped, so its start position is pos1 exactly.
    float3 pos2 = EvaluateBezier(cubic0, SampleT(range0, min(i1 + 1u, range0.y)));
    WorldDistances[i1] = distance(pos1, pos2);
  }
  else
  {
    // The pair straddles a curve boundary: i1 starts its own curve.
    uint2 range1 = CurveIndices[curve1];
    Cubic cubic1 = LoadCubic(curve1);
    float2 t = float2(SampleT(range1, i1), SampleT(range1, min(i1 + 1u, range1.y)));
    WorldDistances[i1] = distance(EvaluateBezier(cubic1, t.x), EvaluateBezier(cubic1, t.y));
  }
}
