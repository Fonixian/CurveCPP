#define GROUP_SIZE 1024u

cbuffer ScanConstants : register(b0) {
    uint ElementCount;
    uint IsTopLevel;
};

RWStructuredBuffer<float2> Values     : register(u0);
RWStructuredBuffer<uint>   Flags      : register(u1);
RWStructuredBuffer<float2> BlockSums  : register(u2);
RWStructuredBuffer<uint>   BlockFlags : register(u3);
RWStructuredBuffer<uint>   NeedsCarry : register(u4);
