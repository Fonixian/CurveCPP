#include "line_common.hlsli"

LinePSInput Corner(float4 p, float3 color, float2 offset) {
    LinePSInput o;
    o.Position = float4(p.xy + offset * p.w, p.zw);
    o.Color = color;
    return o;
}

[maxvertexcount(4)]
void main(line LineVSOutput v[2], inout TriangleStream<LinePSInput> stream) {
    if (v[0].Style.w == 0.0) return;

    float4 a = v[0].Position, b = v[1].Position;
    float3 ca = v[0].Color, cb = v[1].Color;

    if (a.z < 0.0 && b.z < 0.0) return;
    if (a.z < 0.0) {
        const float t = a.z / (a.z - b.z);
        a = lerp(a, b, t);
        ca = lerp(ca, cb, t);
    }
    else if (b.z < 0.0) {
        const float t = b.z / (b.z - a.z);
        b = lerp(b, a, t);
        cb = lerp(cb, ca, t);
    }

    const float2 wh = v[0].Style.xy;
    const float2 dir = (b.xy / b.w - a.xy / a.w) * wh;
    const float len = length(dir);
    const float2 d = len > 1e-6 ? dir / len : float2(1.0, 0.0);
    const float2 offset = float2(-d.y, d.x) * (2.0 * v[0].Style.z) / wh;

    stream.Append(Corner(a, ca,  offset));
    stream.Append(Corner(a, ca, -offset));
    stream.Append(Corner(b, cb,  offset));
    stream.Append(Corner(b, cb, -offset));
}
