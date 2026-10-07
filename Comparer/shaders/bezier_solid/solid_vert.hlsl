#include "solid_common.hlsli"

// Drawn as DrawInstanced(5, total_points - 1) with a TRIANGLESTRIP:
//
//                0 ----------------- 2
//                |                   |  \
//     start  ->  +  ---- segment --- +    4      <- 4: extra vertex that fills the
//    (sample i)  |                   |  /             outer side of the join at the end
//                1 ----------------- 3
//                               end (sample i+1)
//
// Vertices 0, 1 sit on the segment's start ("near side"), 2, 3, 4 on its end ("far side").
// Each vertex works in a LOCAL frame centered on the end point it sits on (the "corner"):
//
//     prevCorner  ----------->  corner  ----------->  nextCorner
//                 dirIn                 dirOut
//
// so for near-side vertices the segment is seen backwards, and "right" means the opposite
// physical side from the near side. In its local frame, 0, 3 and 4 offset to the right and
// 1, 2 to the left - which physically puts 0 and 2 on one side of the stroke, 1 and 3 on the other.

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

StructuredBuffer<uint>   CurveBegins        : register(t1); // one bit per sample: set where a new curve chain begins
StructuredBuffer<float3> CurveControlPoints : register(t3);
StructuredBuffer<uint>   BezierIndexMap     : register(t4);
StructuredBuffer<uint4>  CurveColors        : register(t5); // color1, color2, height1, height2 (for interpolation based on height)
StructuredBuffer<uint>   CurveStyles        : register(t6); // width << 24 | front_cap << 16 | back_cap << 8 | join
StructuredBuffer<uint2>  CurveIndices       : register(t7); // FirstIndex, LastIndex: the curve's sample range, e.g. [0, 50] [51, 90]

struct Cubic {
    float3 P0, P1, P2, P3;
};

Cubic LoadCubic(uint curveIndex) {
    uint k = curveIndex << 2u;
    Cubic c;
    c.P0 = CurveControlPoints[k];
    c.P1 = CurveControlPoints[k + 1u];
    c.P2 = CurveControlPoints[k + 2u];
    c.P3 = CurveControlPoints[k + 3u];
    return c;
}

float3 EvaluateBezier(Cubic c, float t) {
    // Bernstein form
    float s   = 1.0 - t;
    float st3 = 3.0 * s * t;
    return mad(c.P3, t * t * t, mad(c.P2, st3 * t, mad(c.P1, st3 * s, c.P0 * (s * s * s))));
}

// [range.x, range.y] -> [0.0, 1.0]
float SampleT(uint2 range, uint sampleIndex) {
    return float(sampleIndex - range.x) / float(range.y - range.x);
}

// Blends color based on clipping and height or t
float4 CalculateColor(uint4 colorData, float2 height, float2 t, float t0, float t1, bool nearSide) {
    float minHeight = asfloat(colorData.z);
    float maxHeight = asfloat(colorData.w);
    float3 colorA = UnpackColorBits(colorData.x).rgb;
    float3 colorB = UnpackColorBits(colorData.y).rgb;
    float2 blendAtEnds = (minHeight < maxHeight) ? saturate((height - minHeight) / (maxHeight - minHeight)) : t;
    float blend = lerp(blendAtEnds.x, blendAtEnds.y, nearSide ? t0 : t1);
    return float4(lerp(colorA, colorB, blend), 1.0);
}

bool IsCurveBegin(uint2 words, uint wordBase, uint sampleIndex) {
    uint w = ((sampleIndex >> 5u) == wordBase) ? words.x : words.y;
    return ((w >> (sampleIndex & 31u)) & 1u) != 0u;
}

float4 SidePlaneDistances(float4 p) { return mad(p.xxyy, float4(1.0, -1.0, 1.0, -1.0), p.wwww); }
float2 DepthPlaneDistances(float4 p) { return float2(p.z, p.w - p.z); }

float Max4(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }
float Min4(float4 v) { return min(min(v.x, v.y), min(v.z, v.w)); }
float Max2(float2 v) { return max(v.x, v.y); }
float Min2(float2 v) { return min(v.x, v.y); }

// Clips the segment start -> end against the frustum.
bool ClipSegment(inout float4 start, inout float4 end, out float t0, out float t1) {
    t0 = 0.0;
    t1 = 1.0;

    if (isnan(start.x) || isnan(end.x)) return false;

    float4 sideStart = SidePlaneDistances(start), sideEnd = SidePlaneDistances(end);
    float2 depthStart = DepthPlaneDistances(start), depthEnd = DepthPlaneDistances(end);

    if (any(min(sideStart, sideEnd) < 0.0) || any(min(depthStart, depthEnd) < 0.0)) {
        if (any(max(sideStart, sideEnd) < 0.0) || any(max(depthStart, depthEnd) < 0.0)) return false;

        float4 sideDelta = sideEnd - sideStart;
        float4 sideT = -sideStart / sideDelta;
        t0 = max(0.0, Max4((sideDelta > 0.0) ? sideT : 0.0));
        t1 = min(1.0, Min4((sideDelta < 0.0) ? sideT : 1.0));

        float2 depthDelta = depthEnd - depthStart;
        float2 depthT = -depthStart / depthDelta;
        t0 = max(t0, Max2((depthDelta > 0.0) ? depthT : 0.0));
        t1 = min(t1, Min2((depthDelta < 0.0) ? depthT : 1.0));

        if (t0 >= t1) return false;

        float4 start0 = start, end0 = end;
        start = lerp(start0, end0, t0);
        end = lerp(start0, end0, t1);
    }

    return true;
}

bool InnerMiterOverlaps(float2 dirAB, float cosTurn, float2 bisector, float lengthAB, float lengthCB, float lineWidth) {
    if (cosTurn <= -0.9996) return true;

    float cosABC = abs(dot(dirAB, bisector));
    float invSinABC = rsqrt(max(0.0, 1.0 - cosABC * cosABC));
    float l = lineWidth * cosABC * invSinABC;
    return l > lengthAB || l > lengthCB;
}

SolidVSOutput main(uint vertexId : SV_VertexID, uint sampleIndex : SV_InstanceID) {
    SolidVSOutput o = (SolidVSOutput)0;
    bool nearSide = vertexId < 2u;

    uint wordBase = sampleIndex >> 5u;
    uint2 beginWords = uint2(CurveBegins[wordBase], CurveBegins[wordBase + 1u]);
    bool hasPrev = sampleIndex > 0u && !IsCurveBegin(beginWords, wordBase, sampleIndex); // segment before the start
    bool hasNext = (sampleIndex + 2u) < TotalPointCount && !IsCurveBegin(beginWords, wordBase, sampleIndex + 2u); // segment after the end
    bool hasNeighbor = nearSide ? hasPrev : hasNext;

    if ((sampleIndex + 1u) >= TotalPointCount || IsCurveBegin(beginWords, wordBase, sampleIndex + 1u)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    uint currentCurve = BezierIndexMap[sampleIndex];
    Cubic currentCubic = LoadCubic(currentCurve);
    uint2 currentRange = CurveIndices[currentCurve];

    float tStart = SampleT(currentRange, sampleIndex);
    float tEnd = SampleT(currentRange, sampleIndex + 1u);
    float3 startWorld = EvaluateBezier(currentCubic, tStart);
    float3 endWorld = EvaluateBezier(currentCubic, tEnd);

    // The neighbor normally lies on the same curve, but we allow merging neighboring curves
    // When merging two curves we assume one's end position is the same as the other's start
    uint neighborSample = sampleIndex + (nearSide ? -1u : 2u);
    float3 neighborWorld;
    [branch]
    if (hasNeighbor && (nearSide ? (sampleIndex == currentRange.x) : ((sampleIndex + 1u) == currentRange.y))) { // Crosses curve
        uint neighborCurve = currentCurve + (nearSide ? -1u : 1u);
        neighborWorld = EvaluateBezier(LoadCubic(neighborCurve), SampleT(CurveIndices[neighborCurve], neighborSample));
    } else
        neighborWorld = EvaluateBezier(currentCubic, SampleT(currentRange, neighborSample));

    float4 startClip = mul(float4(startWorld, 1.0), VP);
    float4 endClip = mul(float4(endWorld, 1.0), VP);
    float4 neighborClip = mul(float4(neighborWorld, 1.0), VP);

    float t0, t1;
    if (!ClipSegment(startClip, endClip, t0, t1)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    float4 cornerClip = nearSide ? startClip : endClip;
    float4 otherClip = nearSide ? endClip : startClip;

    // If the frustum cut off this vertex's end, the real neighbor is off screen: put it on the
    // other end, which makes this end look like a curve terminus
    if (nearSide ? (t0 > 0.0) : (t1 < 1.0))
        neighborClip = otherClip;

    // Pull the neighbor in front of the near plane.
    if (Min2(neighborClip.zw) < 0.0) {
        float2 toCorner = cornerClip.zw - neighborClip.zw;
        float2 tPlane = -neighborClip.zw / toCorner;
        float pull = max(0.0, max(toCorner.x > 0.0 ? tPlane.x : 0.0, toCorner.y > 0.0 ? tPlane.y : 0.0));
        if (pull > 0.0)
            neighborClip = lerp(neighborClip, cornerClip, pull);
    }

    uint style = CurveStyles[currentCurve];
    float halfWidth = HalfWidth(style);

    o.Color = CalculateColor(CurveColors[currentCurve], float2(startWorld.y, endWorld.y), float2(tStart, tEnd), t0, t1, nearSide);
    uint capCapJoin = Join(style);
    capCapJoin |= (hasPrev ? CurveEndJoined : FrontCap(style)) << 16;
    capCapJoin |= (hasNext ? CurveEndJoined : BackCap(style)) << 8;
    o.CapCapJoin = capCapJoin;

    float3 startNdc = startClip.xyz / startClip.w;
    float3 endNdc = endClip.xyz / endClip.w;
    float2 neighborNdc = neighborClip.xy / neighborClip.w;

    o.Position = float4(nearSide ? startNdc : endNdc, 1.0);

    float2 startPx = mad(startNdc.xy, 0.5, 0.5) * WH;
    float2 endPx = mad(endNdc.xy, 0.5, 0.5) * WH;
    float2 neighborPx = mad(neighborNdc.xy, 0.5, 0.5) * WH;

    float2 corner = nearSide ? startPx : endPx;
    float2 prevCorner = nearSide ? endPx : startPx;
    float2 nextCorner = (hasNeighbor && !isnan(neighborPx.x)) ? neighborPx : prevCorner;

    float nextLength = distance(nextCorner, corner);
    float currentLength = distance(corner, prevCorner);
    float2 dirOut = (nextCorner - corner) / nextLength;
    float2 dirIn = (corner - prevCorner) / currentLength;

    float2 rightOut = float2(dirOut.y, -dirOut.x);
    float2 rightIn = float2(dirIn.y, -dirIn.x);

    float cosTurn = dot(dirOut, dirIn); // 1 = straight on, -1 = full reversal
    float sideSign = (vertexId == 1u || vertexId == 2u) ? -1.0 : 1.0;

    float2 offset;
    if (cosTurn >= 0.0) {
        // Miter
        offset = sideSign * ((rightOut + rightIn) / (1.0 + cosTurn));
    } else {
        float2 miterRight = (cosTurn <= -0.9999) ? rightOut : (rightOut + rightIn) / (1.0 + cosTurn);
        bool innerOnRight = dot(rightOut, dirIn) < 0.0;
        float2 innerMiter = innerOnRight ? miterRight : -miterRight;
        float2 bisector = normalize(miterRight);

        if (InnerMiterOverlaps(dirOut, cosTurn, bisector, nextLength, currentLength, halfWidth)) {
            // Inner miter would reach past a neighboring segment (or full reversal):
            // extend this segment halfWidth past the corner and let the pixel shader shape the end.
            offset = dirIn + sideSign * rightIn;
        } else {
            bool vertexOnRight = (vertexId == 0u) || (vertexId == 3u);
            if (vertexId == 4u || (vertexOnRight != innerOnRight)) {
                // Bevel halfWidth away from the corner on the outer side of the turn
                float cosHalf = clamp(dot(rightIn, bisector), -1.0, 1.0);
                float tanHalf = sqrt(max(0.0, 1.0 - cosHalf) / (1.0 + cosHalf));
                bool onNeighborEdge = vertexId >= 4u;
                float2 edgeNormal = onNeighborEdge ? rightOut : rightIn;
                float2 edgeDir = onNeighborEdge ? -dirOut : dirIn;
                offset = (innerOnRight ? -edgeNormal : edgeNormal) + tanHalf * edgeDir;
            } else {
                // The inner part of the bevel join is just one vertex innerMiter
                offset = innerMiter;
            }
        }
    }

    // Arc position: measured inward from the corner, then flipped on the far side so it runs
    // 0 at the start .. currentLength at the end for every vertex.
    // arc values:
    //   < 0        0            currentLength   > currentLength
    //    |  -----  *  ------------->  *  ----------  |
    //            start               end
    float arc = dot(offset, dirIn) * -halfWidth;
    if (vertexId >= 2u) arc = currentLength - arc;

    // Lateral position: +-halfWidth on the strip's two edges (0, 2 one side, 1, 3 the other);
    // vertex 4 is projected onto this segment's normal.
    o.SDF.x = (vertexId == 4u) ? (dot(offset, rightIn) * -halfWidth)
                               : (((vertexId & 1u) == 0u) ? halfWidth : -halfWidth);
    o.SDF.y = arc;
    o.SDF.zw = float2(halfWidth, currentLength);

    // Offset in pixels -> NDC coordinate.
    o.Position.xy = mad((2.0 * halfWidth) / WH, offset, o.Position.xy);

    return o;
}
