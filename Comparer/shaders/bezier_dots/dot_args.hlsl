#include "dot_common.hlsli"

// Turns the dot total dot_ini accumulated into the argument buffer for DrawInstancedIndirect, so the
// instance count never has to travel to the CPU and back. One thread, four uints.
//
// DrawArgs is a RWByteAddressBuffer and not a RWStructuredBuffer because D3D11 forbids
// D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS together with D3D11_RESOURCE_MISC_BUFFER_STRUCTURED - see
// IndirectDrawArgs in gpu_buffers.h.

cbuffer DotCapacity : register(b1)
{
    uint  Capacity;
    uint3 CapacityPadding;
};

StructuredBuffer<uint> DotCounter : register(t0);

RWByteAddressBuffer DrawArgs : register(u0);

// One screen-aligned quad per dot as a triangle strip; must match dotVertexCount in bezier_dots.cpp
// and dot_vert.hlsl's corner numbering.
static const uint DotVertexCount = 4u;

[numthreads(1, 1, 1)]
void main()
{
    // Belt and braces: the dots buffer is sized from a CPU-side upper bound that cannot be smaller
    // than this count, so the clamp should never bite. If it ever does, the tail of the dots is left
    // undrawn rather than instanced against slots dot_calc could not write.
    uint dotCount = min(DotCounter[0], Capacity);

    // VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation
    DrawArgs.Store4(0, uint4(DotVertexCount, dotCount, 0u, 0u));
}
