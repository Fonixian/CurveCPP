#include "CurveCommon.hlsli"

cbuffer CameraData : register(b0)
{
  float4x4 VP;
  float2 WH;
  uint TotalPointCount;
  uint TotalCurveCount;
};

StructuredBuffer<uint2> CurveIndices : register(t0);
StructuredBuffer<float4> Distances : register(t1); // cumulative arcs as pairs, see CurveCommon.hlsli
StructuredBuffer<PatternStyle> CurveStyles : register(t2);

RWStructuredBuffer<uint> PatternOffsets : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
  uint curveIndex = dispatchThreadId.x;
  if (curveIndex >= TotalCurveCount) return;

  uint2 range = CurveIndices[curveIndex];
  float spacing = CurveStyles[curveIndex].spacing;

  uint dotCount = PairGridCount(WorldArc(Distances[range.y]), spacing) - PairGridCount(WorldArc(Distances[range.x]), spacing);

  PatternOffsets[curveIndex] = dotCount;
}
