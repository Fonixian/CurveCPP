#include "LineCommon.hlsli"

float4 main(LineVSOutput input) : SV_Target
{
  return input.Color;
}
