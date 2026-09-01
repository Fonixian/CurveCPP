// Step 1 of the curve pipeline: evaluate every sample point of every curve, its colour, and the
// length of the segment that follows it - in BOTH world and screen space.
//
// The two length outputs are then turned into cumulative arc length by two segmented prefix sums
// (SegmentedScan), which is why this writes the per-segment length at the point it starts from and
// writes zero at the last point of a curve.
#include "curve_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2   WH;
    uint     TotalPointCount;
    uint     TotalCurveCount;
};

StructuredBuffer<BezierCurveData> BezierData     : register(t0);
StructuredBuffer<uint>            BezierIndexMap : register(t1); // One uint32 curve index per point

RWStructuredBuffer<float4> CalculatedPoints : register(u0);
RWStructuredBuffer<float>  Distances        : register(u1);
RWStructuredBuffer<float>  DistancesScreen  : register(u2);

float3 EvaluateBezier(float3 p0, float3 p1, float3 p2, float3 p3, float t)
{
    float omt = 1.0 - t;
    float omt2 = omt * omt;
    float t2 = t * t;
    return omt2 * omt * p0
         + 3.0 * omt2 * t * p1
         + 3.0 * omt * t2 * p2
         + t2 * t * p3;
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint pointIndex = dispatchId.x;

    // The buffers are over-allocated to a power of two, so GetDimensions() would report the
    // allocation. The live count comes from the constant buffer instead.
    if (pointIndex >= TotalPointCount) return;

    uint curveIndex = BezierIndexMap[pointIndex];
    BezierCurveData bez = BezierData[curveIndex];

    uint firstIndex = (uint)bez.FirstIndex;
    uint lastIndex  = (uint)bez.LastIndex;
    uint resolution = lastIndex - firstIndex + 1;

    float t = float(pointIndex - firstIndex) / float(resolution - 1);
    float3 position = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, t);

    float dist = 0.0;
    float dist_screen = 0.0;
    if (pointIndex < lastIndex)
    {
        float tNext = float(pointIndex - firstIndex + 1) / float(resolution - 1);
        float3 nextPosition = EvaluateBezier(bez.P0, bez.P1, bez.P2, bez.P3, tNext);
        dist = distance(position, nextPosition);

        float4 a = float4(position, 1.0);
        float4 b = float4(nextPosition, 1.0);

        a = mul(a, VP);
        b = mul(b, VP);

        a.xy /= a.w;
        b.xy /= b.w;

        float2 aScreen = (a.xy * 0.5 + 0.5) * WH;
        float2 bScreen = (b.xy * 0.5 + 0.5) * WH;
        dist_screen = distance(aScreen, bScreen);
    }
    Distances[pointIndex] = dist;
    DistancesScreen[pointIndex] = dist_screen;

    // Colour blend factor: world height when a height range was given, otherwise the curve
    // parameter. Height mode clamps, so points outside the band take the end colours flat.
    float blend = t;
    if (bez.MinHeight < bez.MaxHeight)
        blend = saturate((position.y - bez.MinHeight) / (bez.MaxHeight - bez.MinHeight));

    float4 colorBegin = UnpackColorBits(bez.ColorBegin);
    float4 colorEnd   = UnpackColorBits(bez.ColorEnd);
    float4 color = lerp(colorBegin, colorEnd, blend);

    CalculatedPoints[pointIndex] = float4(position, asfloat(PackColorBits(color)));
}
