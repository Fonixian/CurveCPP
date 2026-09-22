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
StructuredBuffer<float>           WorldDistances : register(t1);
StructuredBuffer<CurveStyle>      CurveStyles    : register(t2);

RWStructuredBuffer<uint> PatternOffsets : register(u0);

uint pattern_count(float arc, float spacing) {
    return (arc > 0.0 && spacing > 0.0) ? (uint) floor(arc / spacing) + 1u : 0u;
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex = dispatchThreadId.x;
    if (curveIndex >= TotalCurveCount) return;

    
    float prev_arc = WorldDistances[BezierData[curveIndex].FirstIndex];
    float current_arc = WorldDistances[BezierData[curveIndex].LastIndex];
    float spacing = CurveStyles[curveIndex].Spacing;
    
    uint dot_count = pattern_count(current_arc, spacing) - pattern_count(prev_arc, spacing);

    PatternOffsets[curveIndex] = dot_count;
}