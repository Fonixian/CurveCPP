#include "curve_common.hlsli"

// Pass 1 of 2 in the deterministic pattern layout: count only, no offsets.
//
// This used to hand each curve its base offset with an InterlockedAdd on a
// shared counter, which made a curve's slice depend on the order the thread
// groups retired - the same scene could lay its patterns out differently from
// one frame or one machine to the next. Now each curve only writes its own
// count and ParalellScan turns the counts into offsets, in place.
//
// There is no third pass any more. curve_pattern_resolve existed to fold those
// offsets back into a uint2's .x and to add the last offset to the last count
// for a grand total; ParalellScan's appendTotal writes that total itself, one
// slot past the last curve, and everything downstream recovers the count it
// needs as offsets[i + 1] - offsets[i]. Which is why the count is written ONCE
// here, into the buffer the scan consumes, and nowhere else.

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData     : register(t0);
// World arc length only. The count is camera-independent, so the screen channel - now a buffer of
// its own rather than the .y of this one - is not bound to this pass at all.
StructuredBuffer<float>           WorldDistances : register(t1);
StructuredBuffer<CurveStyle>      CurveStyles    : register(t2);

// Scanned in place by ParalellScan straight after this pass. The counts do not
// survive that, and do not need to: an exclusive scan with the total appended
// leaves every count recoverable as the gap between neighbouring offsets.
RWStructuredBuffer<uint> PatternOffsets : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    // WorldDistances is a CHAIN-cumulative prefix sum - bezier_common sets ONE begin bit for the whole
    // scene - so a curve does not start its pattern at zero. The grid of centres belongs to the
    // chain, at world distances n * spacing measured from the chain's origin, and each curve takes
    // the WINDOW of that grid falling inside its own arc span:
    //
    //     n in [ floor(arcBegin / spacing) + 1 , floor(arcEnd / spacing) ]
    //
    // Half-open at the low end, so a centre landing exactly on a joint belongs to the curve that
    // ENDS there and no centre is counted twice. The first curve has arcBegin == 0 and keeps n = 0
    // as well. This window is what makes the pattern read as ONE dash sequence across merged
    // curves, and it is the same window dot_ini.hlsl counts.
    float arcBegin     = WorldDistances[BezierData[curveIndex].FirstIndex];
    float arcEnd       = WorldDistances[BezierData[curveIndex].LastIndex];
    float curveSpacing = CurveStyles[curveIndex].Spacing;

    uint patternBase = (arcBegin > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(arcBegin / curveSpacing) + 1u
        : 0u;

    uint patternEnd = (arcEnd > 0.0 && curveSpacing > 0.0)
        ? (uint) floor(arcEnd / curveSpacing) + 1u
        : 0u;

    // arcEnd >= arcBegin holds for any real prefix sum, so the difference IS the count. The guard is
    // for the one case that breaks it - a NaN arriving from a NaN control point fails every
    // comparison above and can leave patternEnd at 0 under a positive base. An unsigned underflow
    // there would hand pattern_calc a four-billion iteration loop, so it is worth one compare.
    uint patternCount = (patternEnd > patternBase) ? (patternEnd - patternBase) : 0u;

    PatternOffsets[curveIndex] = patternCount;
}
