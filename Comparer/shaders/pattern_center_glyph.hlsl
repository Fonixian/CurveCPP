// Glyph-mode counterpart of pattern_center_calc.hlsl.
//
// Same job - locate every pattern center along the curve - but a different answer. The arc-length
// path only needs to know HOW FAR ALONG the screen the center sits, because its pattern is a cut
// through the curve's own arc-length coordinate. A glyph is a rigid stamp, so it needs a frame:
// where the center lands on screen, and which way the curve is heading there.
//
// Output per pattern: float4(screenX, screenY, tangentX, tangentY), tangent normalized.
// Screen coordinates are y-DOWN so they can be compared directly against SV_Position in the pixel
// shader. A center behind the near plane is pushed far offscreen with a zero tangent, which
// bezier_glyph_ps.hlsl skips.
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

cbuffer DispatchInfo : register(b0)
{
    uint TotalCurveCount;
};

cbuffer CameraData : register(b1)
{
    float4x4 VP;
    float2 wh;
};

StructuredBuffer<BezierCurveData> BezierData       : register(t0);
StructuredBuffer<float>           Distances        : register(t1); // World-space cumulative arc length per sample
StructuredBuffer<float4>          CalculatedPoints : register(t2); // xyz = world position, w = packed color
StructuredBuffer<float>           Spacing          : register(t3); // World-space pattern spacing per curve
StructuredBuffer<uint2>           DotRanges        : register(t4); // Per curve: x = DotFirst, y = DotCount
RWStructuredBuffer<float4>        PatternGlyphs    : register(u0); // xy = screen position, zw = screen tangent

static const float NearEpsilon = 1e-4;

// Matches the mapping bezier_vert.hlsl uses, then flips y so the result is in the same space as
// SV_Position (origin top-left, y increasing downward).
float2 ClipToScreen(float4 clip)
{
    float2 ndc = clip.xy / clip.w;
    return float2((ndc.x * 0.5 + 0.5) * wh.x,
                  (0.5 - ndc.y * 0.5) * wh.y);
}

[numthreads(8, 8, 1)] // 8 threads horizontally per curve, 8 curves vertically per thread group
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint curveIndex    = dispatchThreadId.y;
    uint threadLaneIdx = dispatchThreadId.x;

    if (curveIndex >= TotalCurveCount)
        return;

    uint2 range    = DotRanges[curveIndex];
    uint  dotFirst = range.x;
    uint  dotCount = range.y;

    if (dotCount == 0)
        return;

    BezierCurveData bez            = BezierData[curveIndex];
    uint            sampleStartIdx = (uint)bez.FirstIndex;
    uint            sampleEndIdx   = (uint)bez.LastIndex;
    float           worldSpacing   = Spacing[curveIndex];

    // STRIDE LOOP: Distribute arbitrary number of dots across the 8 available thread lanes
    for (uint localDotIdx = threadLaneIdx; localDotIdx < dotCount; localDotIdx += 8)
    {
        uint  globalDotIdx    = dotFirst + localDotIdx;
        float targetWorldDist = (float)localDotIdx * worldSpacing;

        // Binary search over world-space Distances to locate the segment indices [sampleA, sampleB]
        uint low     = sampleStartIdx;
        uint high    = sampleEndIdx;
        uint sampleA = sampleStartIdx;

        while (low <= high)
        {
            uint mid = (low + high) / 2;
            if (Distances[mid] <= targetWorldDist)
            {
                sampleA = mid;
                low     = mid + 1;
            }
            else
            {
                if (mid == sampleStartIdx) break;
                high = mid - 1;
            }
        }

        uint sampleB = min(sampleA + 1, sampleEndIdx);

        // Calculate segment-local t parameter based on world-space distances
        float distA         = Distances[sampleA];
        float distB         = Distances[sampleB];
        float segmentLength = distB - distA;

        float segmentT = (segmentLength > 0.00001f)
            ? (targetWorldDist - distA) / segmentLength
            : 0.0f;
        segmentT = saturate(segmentT);

        // The world prefix sum measures chord lengths between samples, so lerping the sample
        // positions by the same factor lands exactly on the arc length we searched for.
        float3 worldA = CalculatedPoints[sampleA].xyz;
        float3 worldB = CalculatedPoints[sampleB].xyz;
        float3 worldP = lerp(worldA, worldB, segmentT);

        float4 clipP = mul(float4(worldP, 1.0), VP);
        float4 clipA = mul(float4(worldA, 1.0), VP);
        float4 clipB = mul(float4(worldB, 1.0), VP);

        if (clipP.w < NearEpsilon || clipA.w < NearEpsilon || clipB.w < NearEpsilon)
        {
            // Behind the camera. A zero tangent marks the entry dead.
            PatternGlyphs[globalDotIdx] = float4(-1e6, -1e6, 0.0, 0.0);
            continue;
        }

        float2 screenP = ClipToScreen(clipP);
        float2 screenA = ClipToScreen(clipA);
        float2 screenB = ClipToScreen(clipB);

        float2 tangent = screenB - screenA;
        float  tanLen  = length(tangent);
        tangent = (tanLen > 1e-6) ? tangent / tanLen : float2(1.0, 0.0);

        PatternGlyphs[globalDotIdx] = float4(screenP, tangent);
    }
}
