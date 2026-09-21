#include "dot_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

RWStructuredBuffer<uint> DotIndices : register(u0);
RWByteAddressBuffer      DrawArgs   : register(u1);

[numthreads(1, 1, 1)]
void main()
{
    uint instanceCount = (TotalCurveCount > 0u) ? DotIndices[TotalCurveCount] : 0u;
    DrawArgs.Store4(0, uint4(4u, instanceCount, 0u, 0u));
}
