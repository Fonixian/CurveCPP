#define GROUP_SIZE 1024u

cbuffer ScanConstants : register(b0) {
    uint ElementCount;
    uint IsTopLevel;
};

// The element type is a single float. It used to be float2, so that one scan could carry both the
// world and the screen arc length of the patterned renderer at once; that renderer now keeps the two
// channels in separate buffers and runs two SegmentedScan instances over them, which costs the same
// bandwidth and lets every other user of this scan (bezier_dots, which only ever needed world arc
// length) stop paying for a second channel it fills with zeroes.
RWStructuredBuffer<float> Values     : register(u0);
RWStructuredBuffer<uint>  Flags      : register(u1);
RWStructuredBuffer<float> BlockSums  : register(u2);
RWStructuredBuffer<uint>  BlockFlags : register(u3);
RWStructuredBuffer<uint>  NeedsCarry : register(u4);
