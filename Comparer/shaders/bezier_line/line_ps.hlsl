#include "line_common.hlsli"

float4 main(LinePSInput input) : SV_Target {
    return float4(input.Color, 1.0);
}
