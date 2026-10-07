#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint2>           CurveIndices    : register(t0);
// The only pass that reads both channels, and it reads them in two distinct phases: the binary
// search walks WORLD arc length alone (one float per probe, not a float2 with half of it discarded),
// and only the single bracketing pair it lands on is looked up in SCREEN arc length.
StructuredBuffer<float>           WorldDistances  : register(t1);
StructuredBuffer<float>           ScreenDistances : register(t2);
StructuredBuffer<PatternStyle>    CurveStyles     : register(t3);
// Exclusive scan of the per-curve centre counts, with the grand total appended one slot past the
// last curve. PatternOffsets[i] is curve i's base index into PatternPosition and the gap to
// PatternOffsets[i + 1] is its count - the last curve included, which is what the appended total
// buys. See curve_pattern_ini.hlsl.
StructuredBuffer<uint>            PatternOffsets  : register(t4);

RWStructuredBuffer<float> PatternPosition : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount) return;

    // The CPU sizes PatternPosition from an upper bound that is clamped to maxPatternCount, so the
    // offsets can run past its end. Clamp this curve's slice to the buffer once, here, so a too-small
    // buffer loses the tail of the pattern instead of writing past the end.
    uint capacity, stride;
    PatternPosition.GetDimensions(capacity, stride);
    uint patternFirst = PatternOffsets[curveIndex];
    uint patternCount = min(PatternOffsets[curveIndex + 1u], capacity) - min(patternFirst, capacity);

    if (patternCount == 0) return;

    uint2 range          = CurveIndices[curveIndex];
    uint  sampleStartIdx = range.x;
    uint  sampleEndIdx   = range.y;
    float worldSpacing   = CurveStyles[curveIndex].spacing;

    // The centre grid is CHAIN-global (see curve_pattern_ini.hlsl): centre n sits at world distance
    // n * spacing from the chain's origin, not from this curve's own start. patternBase is the first
    // n this curve owns, recomputed from the same two numbers ini counted from, so slot
    // patternFirst + localIdx always holds grid index patternBase + localIdx.
    float arcBegin = WorldDistances[sampleStartIdx];

    uint patternBase = (arcBegin > 0.0 && worldSpacing > 0.0)
        ? (uint) floor(arcBegin / worldSpacing) + 1u
        : 0u;

    for (uint localIdx = threadLaneIdx; localIdx < patternCount; localIdx += 8)
    {
        uint  globalIdx       = patternFirst + localIdx;

        // One multiply from the grid index, so this lands on exactly the distance curve_ps.hlsl
        // inverts with floor(totalDistance / spacing) - accumulating base * spacing separately would
        // not. Every target is inside (arcBegin, arcEnd] by construction, so the search below always
        // brackets it properly instead of clamping to an end sample.
        float targetWorldDist = (float)(patternBase + localIdx) * worldSpacing;

        uint low     = sampleStartIdx;
        uint high    = sampleEndIdx;
        uint sampleA = sampleStartIdx;

        while (low <= high) {
            uint mid = (low + high) / 2;
            if (WorldDistances[mid] <= targetWorldDist) {
                sampleA = mid;
                low     = mid + 1;
            } else {
                if (mid == sampleStartIdx) break;
                high = mid - 1;
            }
        }

        uint sampleB = min(sampleA + 1, sampleEndIdx);

        float distA = WorldDistances[sampleA];
        float distB = WorldDistances[sampleB];
        float segmentLength = distB - distA;

        float segmentT = (segmentLength > 0.00001f)
            ? (targetWorldDist - distA) / segmentLength
            : 0.0f;

        float screenDistA = ScreenDistances[sampleA];
        float screenDistB = ScreenDistances[sampleB];

        PatternPosition[globalIdx] = lerp(screenDistA, screenDistB, saturate(segmentT));
    }
}
