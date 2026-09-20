#include "curve_common.hlsli"

// Pass 1 of 3 in the deterministic pattern layout: count only, no offsets.
//
// This used to hand each curve its base offset with an InterlockedAdd on a
// shared counter, which made a curve's slice depend on the order the thread
// groups retired - the same scene could lay its patterns out differently from
// one frame or one machine to the next. Now each curve only writes its own
// count and ParalellScan turns the counts into offsets (curve_pattern_resolve).

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData  : register(t0);
StructuredBuffer<float2>          Distances   : register(t1);
StructuredBuffer<CurveStyle>      CurveStyles : register(t2);

// Scanned in place by ParalellScan straight after this pass, so the count has
// to survive somewhere else too - hence the second copy in PatternRanges.y.
// That duplicate uint per curve is the whole memory cost of dropping the atomic.
RWStructuredBuffer<uint>  PatternOffsets : register(u0);
RWStructuredBuffer<uint2> PatternRanges  : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    // Distances is a CHAIN-cumulative prefix sum - bezier_common sets ONE begin bit for the whole
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
    float arcBegin     = Distances[BezierData[curveIndex].FirstIndex].x;
    float arcEnd       = Distances[BezierData[curveIndex].LastIndex].x;
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

    // .x is left at 0 and filled in by curve_pattern_resolve once the scan has run.
    PatternRanges[curveIndex] = uint2(0u, patternCount);
}
