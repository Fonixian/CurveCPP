#include "line_common.hlsli"

float4 main(LineVSOutput input) : SV_Target {
    return float4(input.Color, 1.0);
}
