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

StructuredBuffer<BezierCurveData> BezierData      : register(t0);
StructuredBuffer<float>           Distances       : register(t1); // World-space cumulative arc length per sample
StructuredBuffer<float>           DistancesScreen : register(t2); // Screen-space cumulative arc length per sample
StructuredBuffer<float>           Spacing         : register(t3); // World-space dot spacing per curve
StructuredBuffer<uint2>           DotRanges       : register(t4); // Per curve: x = DotFirst, y = DotCount
RWStructuredBuffer<float>         PatternPosition : register(u0); // Screen-space distance output per dot

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

    BezierCurveData bez      = BezierData[curveIndex];
    uint            sampleStartIdx = (uint)bez.FirstIndex;
    uint            sampleEndIdx   = (uint)bez.LastIndex;
    float           worldSpacing   = Spacing[curveIndex];

    // STRIDE LOOP: Distribute arbitrary number of dots across the 8 available thread lanes
    for (uint localDotIdx = threadLaneIdx; localDotIdx < dotCount; localDotIdx += 8)
    {
        uint  globalDotIdx     = dotFirst + localDotIdx;
        float targetWorldDist  = (float)localDotIdx * worldSpacing;

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
        float distA = Distances[sampleA];
        float distB = Distances[sampleB];
        float segmentLength = distB - distA;

        float segmentT = (segmentLength > 0.00001f) 
            ? (targetWorldDist - distA) / segmentLength 
            : 0.0f;

        // Interpolate screen-space distance using world-space lerp factor t
        float screenDistA = DistancesScreen[sampleA];
        float screenDistB = DistancesScreen[sampleB];

        float finalScreenDist = lerp(screenDistA, screenDistB, saturate(segmentT));

        PatternPosition[globalDotIdx] = finalScreenDist;
    }
}