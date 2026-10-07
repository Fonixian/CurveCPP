#include "curve_common.hlsli"

cbuffer CameraData : register(b1) {
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};


StructuredBuffer<float>        WorldDistances     : register(t0); // arc length from the chain's start, per sample
StructuredBuffer<uint>         CurveBegins        : register(t1);
StructuredBuffer<float>        ScreenDistances    : register(t2); // same in pixels
StructuredBuffer<float3>       CurveControlPoints : register(t3);
StructuredBuffer<uint>         BezierIndexMap     : register(t4);
StructuredBuffer<uint4>        CurveColors        : register(t5);
StructuredBuffer<PatternStyle> CurveStyles        : register(t6);
StructuredBuffer<uint2>        CurveIndices       : register(t7);
StructuredBuffer<uint>         PatternOffsets     : register(t8);
StructuredBuffer<uint2>        CurveChains        : register(t9);

Cubic LoadCubic(uint curveIndex) {
    uint k = curveIndex << 2u;
    Cubic c;
    c.P0 = CurveControlPoints[k];
    c.P1 = CurveControlPoints[k + 1u];
    c.P2 = CurveControlPoints[k + 2u];
    c.P3 = CurveControlPoints[k + 3u];
    return c;
}

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

void PullInFront(inout float4 neighborClip, float4 cornerClip) {
    if (Min2(neighborClip.zw) < 0.0) {
        float2 toCorner = cornerClip.zw - neighborClip.zw;
        float2 tPlane = -neighborClip.zw / toCorner;
        float pull = max(0.0, max(toCorner.x > 0.0 ? tPlane.x : 0.0, toCorner.y > 0.0 ? tPlane.y : 0.0));
        if (pull > 0.0)
            neighborClip = lerp(neighborClip, cornerClip, pull);
    }
}

static const float BisectorMinCos = 1e-3;
static const float MaxArcShear = 8.0;

float2 SafeDirection(float2 from, float2 to) {
    float2 delta = to - from;
    float len = length(delta);
    return (len > 1e-6) ? (delta / len) : float2(0.0, 0.0);
}

// Signed tan(theta/2) of the turn between dir and neighborDir, seen from the lateral side.
float JointShear(float2 lateralDir, float2 dir, float2 neighborDir) {
    float denom = 1.0 + dot(neighborDir, dir);
    if (!(denom > BisectorMinCos)) return 0.0;
    return clamp(dot(lateralDir, neighborDir) / denom, -MaxArcShear, MaxArcShear);
}

bool InnerMiterOverlaps(float2 dirAB, float cosTurn, float2 bisector, float lengthAB, float lengthCB, float lineWidth) {
    if (cosTurn <= -0.9996) return true;

    float cosABC = abs(dot(dirAB, bisector));
    float invSinABC = rsqrt(max(0.0, 1.0 - cosABC * cosABC));
    float l = lineWidth * cosABC * invSinABC;
    return l > lengthAB || l > lengthCB;
}

PatternedVSOutput main(uint vertexId : SV_VertexID, uint sampleIndex : SV_InstanceID) {
    PatternedVSOutput o = (PatternedVSOutput)0;
    bool nearSide = vertexId < 2u;

    uint wordBase = sampleIndex >> 5u;
    uint2 beginWords = uint2(CurveBegins[wordBase], CurveBegins[wordBase + 1u]);
    bool hasPrev = sampleIndex > 0u && !IsCurveBegin(beginWords, wordBase, sampleIndex);
    bool hasNext = (sampleIndex + 2u) < TotalPointCount && !IsCurveBegin(beginWords, wordBase, sampleIndex + 2u);

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

    // Unlike solid curve case every vertex needs BOTH neighbors
    float3 beforeWorld;
    [branch]
    if (hasPrev && sampleIndex == currentRange.x) {
        uint prevCurve = currentCurve - 1u;
        beforeWorld = EvaluateBezier(LoadCubic(prevCurve), SampleT(CurveIndices[prevCurve], sampleIndex - 1u));
    } else
        beforeWorld = EvaluateBezier(currentCubic, SampleT(currentRange, sampleIndex - 1u));

    float3 afterWorld;
    [branch]
    if (hasNext && (sampleIndex + 1u) == currentRange.y) {
        uint nextCurve = currentCurve + 1u;
        afterWorld = EvaluateBezier(LoadCubic(nextCurve), SampleT(CurveIndices[nextCurve], sampleIndex + 2u));
    } else
        afterWorld = EvaluateBezier(currentCubic, SampleT(currentRange, sampleIndex + 2u));

    float4 beforeClip = mul(float4(beforeWorld, 1.0), VP);
    float4 startClip = mul(float4(startWorld, 1.0), VP);
    float4 endClip = mul(float4(endWorld, 1.0), VP);
    float4 afterClip = mul(float4(afterWorld, 1.0), VP);

    // Where curve_calc_points started measuring this segment's screen length.
    float4 measuredStartClip = ClipToNearPlane(startClip, endClip);

    float t0, t1;
    if (!ClipSegment(startClip, endClip, t0, t1)) {
        o.Position = 0.0 / 0.0;
        return o;
    }

    // No neighbor, or the frustum cut that end off: put the neighbor on the other end. This also
    // makes ArcShear 0 there.
    if (!hasPrev || t0 > 0.0) beforeClip = endClip;
    if (!hasNext || t1 < 1.0) afterClip = startClip;

    PullInFront(beforeClip, startClip);
    PullInFront(afterClip, endClip);

    PatternStyle style = CurveStyles[currentCurve];
    // No solid mode (that is the solid renderer's job): without a positive spacing there is nothing
    // to draw, and the pattern coordinate below would divide by it.
    if (!(style.spacing > 0.0)) {
        o.Position = 0.0 / 0.0;
        return o;
    }
    float halfWidth = HalfWidth(style.width_capcapjoin);
    float cornerT = nearSide ? t0 : t1;

    float startWorldDistance = WorldDistances[sampleIndex];
    float endWorldDistance = WorldDistances[sampleIndex + 1u];

    o.Neighbors = uint2((hasPrev || t0 > 0.0) ? 1u : 0u, (hasNext || t1 < 1.0) ? 1u : 0u);
    o.DashLength = style.dash_length;
    o.CapCapJoin = style.width_capcapjoin; // FrontCap/BackCap/Join ignore the width byte

    // Pattern coordinate. WorldDistances restarts at 0 at each chain, and the chain's centers sit on
    // one grid, n * spacing from its start, so world arc maps to a slot of PatternPosition by
    //
    //     slot = floor(arc / spacing) + (PatternOffsets[curve] - first grid index the curve owns)
    //
    // The divide and the bias are both linear, so they commute with the rasterizer's interpolation
    // and the pixel shader only has to floor() it. The bias is 0 while every curve of the chain has
    // the same spacing; it is there for mixed spacing.
    float curveArcStart = WorldDistances[currentRange.x];
    float patternBase = (curveArcStart > 0.0) ? floor(curveArcStart / style.spacing) + 1.0 : 0.0;
    float patternCoord = lerp(startWorldDistance, endWorldDistance, cornerT) / style.spacing
                       + (float(PatternOffsets[currentCurve]) - patternBase);

    // Chains are runs of consecutive curves and their slots are laid out in curve order, so the
    // chain's slots are one contiguous range.
    uint2 chain = CurveChains[currentCurve];
    o.PatternSlots = uint2(PatternOffsets[chain.x], PatternOffsets[chain.y + 1u]);

    float3 color = CalculateColor(CurveColors[currentCurve], float2(startWorld.y, endWorld.y), float2(tStart, tEnd), t0, t1, nearSide).rgb;
    o.ColorPattern = float4(color, patternCoord);

    float3 startNdc = startClip.xyz / startClip.w;
    float3 endNdc = endClip.xyz / endClip.w;
    float2 beforeNdc = beforeClip.xy / beforeClip.w;
    float2 afterNdc = afterClip.xy / afterClip.w;

    o.Position = float4(nearSide ? startNdc : endNdc, 1.0);

    float2 beforePx = mad(beforeNdc, 0.5, 0.5) * WH;
    float2 startPx = mad(startNdc.xy, 0.5, 0.5) * WH;
    float2 endPx = mad(endNdc.xy, 0.5, 0.5) * WH;
    float2 afterPx = mad(afterNdc, 0.5, 0.5) * WH;

    // The screen arc at the (clipped) start, measured along the screen line from where
    // curve_calc_points started measuring - never lerped by t0, which is not linear on screen.
    o.ScreenArcBegin = ScreenDistances[sampleIndex] + distance(mad(measuredStartClip.xy / measuredStartClip.w, 0.5, 0.5) * WH, startPx);

    // From the segment's own points, not the per-vertex corner frame below, so all five vertices agree.
    {
        float2 segmentDir = SafeDirection(startPx, endPx);
        float2 lateralDir = float2(-segmentDir.y, segmentDir.x); // the +SDF.x side
        o.ArcShear = float2(
            JointShear(lateralDir, segmentDir, SafeDirection(beforePx, startPx)),
            JointShear(lateralDir, segmentDir, SafeDirection(endPx, afterPx)));
    }

    float2 corner = nearSide ? startPx : endPx;
    float2 prevCorner = nearSide ? endPx : startPx;
    float2 nextCorner = nearSide ? beforePx : afterPx;

    float nextLength = distance(nextCorner, corner);
    float currentLength = distance(corner, prevCorner);
    float2 dirOut = (nextCorner - corner) / nextLength;
    float2 dirIn = (corner - prevCorner) / currentLength;

    float2 rightOut = float2(dirOut.y, -dirOut.x);
    float2 rightIn = float2(dirIn.y, -dirIn.x);

    float cosTurn = dot(dirOut, dirIn);
    float sideSign = (vertexId == 1u || vertexId == 2u) ? -1.0 : 1.0;

    float2 offset;
    if (cosTurn >= 0.0) {
        offset = sideSign * ((rightOut + rightIn) / (1.0 + cosTurn));
    } else {
        float2 miterRight = (cosTurn <= -0.9999) ? rightOut : (rightOut + rightIn) / (1.0 + cosTurn);
        bool innerOnRight = dot(rightOut, dirIn) < 0.0;
        float2 innerMiter = innerOnRight ? miterRight : -miterRight;
        float2 bisector = normalize(miterRight);

        if (InnerMiterOverlaps(dirOut, cosTurn, bisector, nextLength, currentLength, halfWidth)) {
            offset = dirIn + sideSign * rightIn;
        } else {
            bool vertexOnRight = (vertexId == 0u) || (vertexId == 3u);
            if (vertexId == 4u || (vertexOnRight != innerOnRight)) {
                float cosHalf = clamp(dot(rightIn, bisector), -1.0, 1.0);
                float tanHalf = sqrt(max(0.0, 1.0 - cosHalf) / (1.0 + cosHalf));
                bool onNeighborEdge = vertexId >= 4u;
                float2 edgeNormal = onNeighborEdge ? rightOut : rightIn;
                float2 edgeDir = onNeighborEdge ? -dirOut : dirIn;
                offset = (innerOnRight ? -edgeNormal : edgeNormal) + tanHalf * edgeDir;
            } else {
                offset = innerMiter;
            }
        }
    }

    float arc = dot(offset, dirIn) * -halfWidth;
    if (vertexId >= 2u) arc = currentLength - arc;

    o.SDF.x = (vertexId == 4u) ? (dot(offset, rightIn) * -halfWidth)
                               : (((vertexId & 1u) == 0u) ? halfWidth : -halfWidth);
    o.SDF.y = arc;
    o.SDF.zw = float2(halfWidth, currentLength);

    o.Position.xy = mad((2.0 * halfWidth) / WH, offset, o.Position.xy);

    return o;
}
