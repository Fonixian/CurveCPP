#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount; // samples, 64 per piece
    uint     TotalCurveCount; // pieces
};

StructuredBuffer<float>           WorldDistances  : register(t0); // arc at each sample, from the chain's start
StructuredBuffer<float>           ScreenDistances : register(t1); // same, px
StructuredBuffer<PatternStyle>    CurveStyles     : register(t2);
StructuredBuffer<uint>            PatternOffsets  : register(t3);

RWStructuredBuffer<float> PatternPosition : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint pieceIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (pieceIndex >= TotalCurveCount) return;

    uint capacity, stride;
    PatternPosition.GetDimensions(capacity, stride);
    uint patternFirst = PatternOffsets[pieceIndex];
    uint patternCount = min(PatternOffsets[pieceIndex + 1u], capacity) - min(patternFirst, capacity);

    if (patternCount == 0) return;

    uint  firstSample  = pieceIndex << PieceSampleShift;
    float worldSpacing = CurveStyles[pieceIndex].spacing;
    float arcBegin     = WorldDistances[firstSample];

    uint patternBase = (arcBegin > 0.0 && worldSpacing > 0.0)
        ? (uint) floor(arcBegin / worldSpacing) + 1u
        : 0u;

    for (uint localIdx = threadLaneIdx; localIdx < patternCount; localIdx += 8)
    {
        float targetWorldDist = (float)(patternBase + localIdx) * worldSpacing;

        // The last sample of the piece at or before the target (the first one if none is), over the
        // fixed 64-sample block: always exactly PieceSampleShift dependent loads, no loop exit.
        uint sampleA = 0u;
        [unroll]
        for (uint step = PieceSamples >> 1u; step > 0u; step >>= 1u) {
            if (WorldDistances[firstSample + sampleA + step] <= targetWorldDist)
                sampleA += step;
        }
        uint sampleB = min(sampleA + 1u, PieceSampleMask);
        sampleA += firstSample;
        sampleB += firstSample;

        float distA = WorldDistances[sampleA];
        float distB = WorldDistances[sampleB];
        float segmentLength = distB - distA;

        float segmentT = (segmentLength > 0.00001f)
            ? (targetWorldDist - distA) / segmentLength
            : 0.0f;

        float screenDistA = ScreenDistances[sampleA];
        float screenDistB = ScreenDistances[sampleB];

        PatternPosition[patternFirst + localIdx] = lerp(screenDistA, screenDistB, saturate(segmentT));
    }
}
