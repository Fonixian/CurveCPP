#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<uint2>           CurveIndices    : register(t0);
StructuredBuffer<float4>          Distances       : register(t1); // cumulative world / screen arcs as pairs (curve_common.hlsli)
StructuredBuffer<uint>            CurveBegins     : register(t2); // one bit per sample: set where a chain starts
StructuredBuffer<PatternStyle>    CurveStyles     : register(t3);
StructuredBuffer<uint>            PatternOffsets  : register(t4);

// The screen arc of every centre as a compensated pair (hi, lo), so curve_ps can take it relative to a
// segment start without losing the low bits on a long chain.
RWStructuredBuffer<float2> PatternPosition : register(u0);

// The LAST centre of every chain is stored with the sign bit of its HIGH part set (screen arcs are never
// negative, so hi never is and the bit is free; lo keeps its own sign); curve_ps reads it as "the next
// slot belongs to another chain". That is the pixel
// shader's only way to find the end of its chain, since it is no longer told the chain's slot range.
//
// A centre is the chain's last when it is the last one of its curve AND no later curve of the same chain
// owns a centre. A curve shorter than the spacing owns none, so the chain can end in a run of empty
// curves; this walks over those and stops at the first curve that has a centre (not the last) or at the
// first curve that starts a new chain, which is where its begin bit is set. A merged curve's first
// sample (its own copy of the joint with the curve before it) carries no begin bit.
// Only the curve's last centre asks, and only once per curve - see main.
bool NoLaterCenterInChain(uint curveIndex)
{
    for (uint next = curveIndex + 1u; next < TotalCurveCount; ++next) {
        uint firstSample = CurveIndices[next].x;
        if (((CurveBegins[firstSample >> 5u] >> (firstSample & 31u)) & 1u) != 0u) return true;
        if (PatternOffsets[next + 1u] != PatternOffsets[next]) return false;
    }
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount) return;

    uint capacity, stride;
    PatternPosition.GetDimensions(capacity, stride);
    uint patternFirst = PatternOffsets[curveIndex];
    uint patternCount = min(PatternOffsets[curveIndex + 1u], capacity) - min(patternFirst, capacity);

    if (patternCount == 0) return;

    uint2 range          = CurveIndices[curveIndex];
    uint  sampleStartIdx = range.x;
    uint  sampleEndIdx   = range.y;
    float worldSpacing   = CurveStyles[curveIndex].spacing;

    // The first grid point this curve owns - the same exact mapping as curve_pattern_ini and curve_vs.
    uint patternBase = PairGridCount(WorldArc(Distances[sampleStartIdx]), worldSpacing);

    // Only this curve's last centre can end its chain, and exactly one lane places it (the loop below
    // strides by 8), so that lane alone walks the following curves, once, before the loop. Truncation by
    // the buffer capacity drops the real last centre, so then nothing is flagged.
    uint lastLocal = patternCount - 1u;
    bool markLast = (threadLaneIdx == (lastLocal & 7u))
        && (PatternOffsets[curveIndex + 1u] - patternFirst == patternCount)
        && NoLaterCenterInChain(curveIndex);

    for (uint localIdx = threadLaneIdx; localIdx < patternCount; localIdx += 8)
    {
        uint  globalIdx       = patternFirst + localIdx;

        // The grid point k * spacing, exactly, as a pair. In plain floats it would be off by up to half an
        // ulp of the chain's arc length - on a long chain a visible fraction of the spacing.
        float2 target = PairFromProduct((float)(patternBase + localIdx), worldSpacing);

        uint low     = sampleStartIdx;
        uint high    = sampleEndIdx;
        uint sampleA = sampleStartIdx;

        while (low <= high) {
            uint mid = (low + high) / 2;
            if (PairLessEqual(WorldArc(Distances[mid]), target)) {
                sampleA = mid;
                low     = mid + 1;
            } else {
                if (mid == sampleStartIdx) break;
                high = mid - 1;
            }
        }

        uint sampleB = min(sampleA + 1, sampleEndIdx);

        // One 16-byte load per end gives both channels. The differences are taken pairwise, so they are
        // as precise as the (small) segment itself, not as the (large) arc it starts at.
        float4 distA = Distances[sampleA];
        float4 distB = Distances[sampleB];
        float segmentLength = PairDiff(WorldArc(distB), WorldArc(distA));

        float segmentT = (segmentLength > 0.00001f)
            ? PairDiff(target, WorldArc(distA)) / segmentLength
            : 0.0f;

        float2 center = PairAdd(ScreenArc(distA), saturate(segmentT) * PairDiff(ScreenArc(distB), ScreenArc(distA)));
        if (markLast && localIdx == lastLocal)
            center.x = asfloat(asuint(center.x) | 0x80000000u);

        PatternPosition[globalIdx] = center;
    }
}
