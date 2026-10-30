#include "CurveCommon.hlsli"

cbuffer CameraData : register(b0)
{
  float4x4 VP;
  float2 WH;
  uint TotalPointCount;
  uint TotalCurveCount;
};

StructuredBuffer<uint2> CurveIndices : register(t0);
StructuredBuffer<float2> Distances : register(t1); // x: cumulative world arc, y: cumulative screen arc
StructuredBuffer<uint> CurveBegins : register(t2); // one bit per sample: set where a chain starts
StructuredBuffer<PatternStyle> CurveStyles : register(t3);
StructuredBuffer<uint> PatternOffsets : register(t4);

RWStructuredBuffer<float> PatternPosition : register(u0);

// The LAST centre of every chain is stored with its sign bit set (screen arcs are never negative, so the
// bit is free); CurvePs reads it as "the next slot belongs to another chain". That is the pixel
// shader's only way to find the end of its chain, since it is no longer told the chain's slot range.
//
// A centre is the chain's last when it is the last one of its curve AND no later curve of the same chain
// owns a centre. A curve shorter than the spacing owns none, so the chain can end in a run of empty
// curves; this walks over those and stops at the first curve that has a centre (not the last) or at the
// first curve that starts a new chain, which is where its begin bit is set. A merged curve's first
// sample is the joint it shares with the curve before it, which carries no begin bit.
// Only the curve's last centre asks, and only once per curve - see main.
bool NoLaterCenterInChain(uint curveIndex)
{
  for (uint next = curveIndex + 1u; next < TotalCurveCount; ++next)
  {
    uint firstSample = CurveIndices[next].x;
    if (((CurveBegins[firstSample >> 5u] >> (firstSample & 31u)) & 1u) != 0u) return true;
    if (PatternOffsets[next + 1u] != PatternOffsets[next]) return false;
  }
  return true;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
  uint curveIndex = dispatchThreadId.y;
  uint threadLaneIdx = dispatchThreadId.x;

  if (curveIndex >= TotalCurveCount) return;

  uint capacity;
  uint stride;
  PatternPosition.GetDimensions(capacity, stride);
  uint patternFirst = PatternOffsets[curveIndex];
  uint patternCount = min(PatternOffsets[curveIndex + 1u], capacity) - min(patternFirst, capacity);

  if (patternCount == 0) return;

  uint2 range = CurveIndices[curveIndex];
  uint sampleStartIdx = range.x;
  uint sampleEndIdx = range.y;
  float worldSpacing = CurveStyles[curveIndex].spacing;

  float arcBegin = Distances[sampleStartIdx].x;

  uint patternBase = (arcBegin > 0.0 && worldSpacing > 0.0)
    ? (uint)floor(arcBegin / worldSpacing) + 1u
    : 0u;

  // Only this curve's last centre can end its chain, and exactly one lane places it (the loop below
  // strides by 8), so that lane alone walks the following curves, once, before the loop. Truncation by
  // the buffer capacity drops the real last centre, so then nothing is flagged.
  uint lastLocal = patternCount - 1u;
  bool markLast = (threadLaneIdx == (lastLocal & 7u))
    && (PatternOffsets[curveIndex + 1u] - patternFirst == patternCount)
    && NoLaterCenterInChain(curveIndex);

  for (uint localIdx = threadLaneIdx; localIdx < patternCount; localIdx += 8)
  {
    uint globalIdx = patternFirst + localIdx;

    float targetWorldDist = (float)(patternBase + localIdx) * worldSpacing;

    uint low = sampleStartIdx;
    uint high = sampleEndIdx;
    uint sampleA = sampleStartIdx;

    while (low <= high)
    {
      uint mid = (low + high) / 2;
      if (Distances[mid].x <= targetWorldDist)
      {
        sampleA = mid;
        low = mid + 1;
      }
      else
      {
        if (mid == sampleStartIdx) break;
        high = mid - 1;
      }
    }

    uint sampleB = min(sampleA + 1, sampleEndIdx);

    // One 8-byte load per end gives both channels.
    float2 distA = Distances[sampleA];
    float2 distB = Distances[sampleB];
    float segmentLength = distB.x - distA.x;

    float segmentT = (segmentLength > 0.00001f)
      ? (targetWorldDist - distA.x) / segmentLength
      : 0.0f;

    float center = lerp(distA.y, distB.y, saturate(segmentT));
    if (markLast && localIdx == lastLocal)
    {
      center = asfloat(asuint(center) | 0x80000000u);
    }

    PatternPosition[globalIdx] = center;
  }
}
