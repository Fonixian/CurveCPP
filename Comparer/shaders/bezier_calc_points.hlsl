struct BezierCurveData
{
    float3 P0;
    int    FirstIndex;
    float3 P1;
    int    LastIndex;
    float3 P2;
    int    ColorBegin;
    float3 P3;
    int    ColorEnd;
};

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2 wh;
};

StructuredBuffer<BezierCurveData> BezierData : register(t0);
StructuredBuffer<uint> BezierIndexMap : register(t1);

RWStructuredBuffer<float4> CalculatedPoints : register(u0);
RWStructuredBuffer<float> Distances : register(u1);
RWStructuredBuffer<float> DistancesScreen : register(u2);

float4 UnpackColor(int packed)
{
    uint p = asuint(packed);
    float r = float(p & 0xFF) / 255.0;
    float g = float((p >> 8) & 0xFF) / 255.0;
    float b = float((p >> 16) & 0xFF) / 255.0;
    float a = float((p >> 24) & 0xFF) / 255.0;
    return float4(r, g, b, a);
}

uint PackColor(float4 color)
{
    uint r = uint(saturate(color.r) * 255.0 + 0.5);
    uint g = uint(saturate(color.g) * 255.0 + 0.5);
    uint b = uint(saturate(color.b) * 255.0 + 0.5);
    uint a = uint(saturate(color.a) * 255.0 + 0.5);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

uint UnpackCurveIndex(uint pointIndex)
{
    uint packed = BezierIndexMap[pointIndex >> 1];
    return (pointIndex & 1) ? (packed >> 16) : (packed & 0xFFFF);
}

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
    uint pointCount, pointStride;
    CalculatedPoints.GetDimensions(pointCount, pointStride);

    uint pointIndex = dispatchId.x;
    if (pointIndex >= pointCount) return;

    uint curveIndex = UnpackCurveIndex(pointIndex);
    BezierCurveData bez = BezierData[curveIndex];

    uint firstIndex = (uint)bez.FirstIndex;
    uint lastIndex = (uint)bez.LastIndex;
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
        
        float2 aScreen = (a.xy * 0.5 + 0.5) * wh;
        float2 bScreen = (b.xy * 0.5 + 0.5) * wh;
        dist_screen = distance(aScreen, bScreen);
    }
    Distances[pointIndex] = dist;
    DistancesScreen[pointIndex] = dist_screen;

    float4 colorBegin = UnpackColor(bez.ColorBegin);
    float4 colorEnd = UnpackColor(bez.ColorEnd);
    float4 color = lerp(colorBegin, colorEnd, t);

    CalculatedPoints[pointIndex] = float4(position, asfloat(PackColor(color)));
}