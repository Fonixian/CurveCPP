#include "dot_common.hlsli"

cbuffer CameraData : register(b0)
{
    float4x4 VP;
    float2 WH;
    uint TotalPointCount;
    uint TotalCurveCount;
};

cbuffer DotCapacity : register(b1)
{
    uint  Capacity;
    uint3 CapacityPadding;
};

RWStructuredBuffer<uint> DotIndices : register(u0);
RWByteAddressBuffer      DrawArgs   : register(u1);

[numthreads(1, 1, 1)]
void main()
{
    // With no curves there is no appended total to read - DotIndices[0] would be whatever the last
    // scene left behind.
    uint instanceCount = (TotalCurveCount > 0u)
        ? min(DotIndices[TotalCurveCount], Capacity)
        : 0u;

    DrawArgs.Store4(0, uint4(4u, instanceCount, 0u, 0u));
}
