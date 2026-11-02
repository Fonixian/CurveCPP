#include "App.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <imgui.h>
#include "imgui_impl_sdl3.h"
#include "imgui_impl_dx11.h"
#include "Include/Axodox.Storage.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

using namespace Axodox::Storage;
using namespace Axodox::Graphics;
using namespace DirectX;

namespace
{
  constexpr float Pi = 3.14159265359f;
  constexpr float Tau = 6.28318530718f;

  constexpr float WaterLevel = -3.1f;
  constexpr float TerrainExtent = 56.0f;
  constexpr int TerrainCells = 320;

  const XMFLOAT3 PlanetCenter = { 0.0f, 3.0f, 0.0f };
  constexpr float PlanetRadius = 2.0f;

  const XMFLOAT3 SunDirection = { 0.30f, 0.12f, -1.0f };

  // --- small vector helpers ----------------------------------------------------------------
  XMFLOAT3 operator+(const XMFLOAT3& a, const XMFLOAT3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
  XMFLOAT3 operator-(const XMFLOAT3& a, const XMFLOAT3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
  XMFLOAT3 operator*(const XMFLOAT3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }

  float Dot(const XMFLOAT3& a, const XMFLOAT3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
  float Length(const XMFLOAT3& a) { return sqrtf(Dot(a, a)); }
  XMFLOAT3 Normalize(const XMFLOAT3& a) { return a * (1.0f / std::max(Length(a), 1e-12f)); }

  XMFLOAT3 Transform(const XMFLOAT3& p, const XMMATRIX& m)
  {
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3Transform(XMLoadFloat3(&p), m));
    return result;
  }

  XMFLOAT3 TransformDir(const XMFLOAT3& p, const XMMATRIX& m)
  {
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3TransformNormal(XMLoadFloat3(&p), m));
    return result;
  }

  float Fract(float x) { return x - floorf(x); }
  float Smoothstep(float a, float b, float x)
  {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  }

  XMFLOAT4 HueColor(float hue, float alpha = 1.0f)
  {
    return {
      0.5f + 0.5f * cosf(Tau * (hue + 0.00f)),
      0.5f + 0.5f * cosf(Tau * (hue + 0.33f)),
      0.5f + 0.5f * cosf(Tau * (hue + 0.67f)),
      alpha
    };
  }

  XMFLOAT4 WithAlpha(XMFLOAT4 c, float a)
  {
    c.w = a;
    return c;
  }

  XMFLOAT4 Scale3(XMFLOAT4 c, float s)
  {
    return { c.x * s, c.y * s, c.z * s, c.w };
  }

  float Hash(int x, int y)
  {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return float(h & 0xFFFFFFu) / 16777216.0f;
  }

  float Hash1(int i) { return Hash(i, 0x51ED); }

  float ValueNoise(float x, float y)
  {
    const int ix = int(floorf(x));
    const int iy = int(floorf(y));
    float fx = x - float(ix);
    float fy = y - float(iy);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float a = Hash(ix, iy);
    const float b = Hash(ix + 1, iy);
    const float c = Hash(ix, iy + 1);
    const float d = Hash(ix + 1, iy + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
  }

  float Fbm(float x, float y, int octaves)
  {
    float sum = 0.0f;
    float amplitude = 0.5f;
    float norm = 0.0f;
    for (int i = 0; i < octaves; ++i)
    {
      sum += amplitude * ValueNoise(x, y);
      norm += amplitude;
      x = x * 2.03f + 17.1f;
      y = y * 2.03f - 9.7f;
      amplitude *= 0.5f;
    }
    return sum / norm;
  }

  // Valley with a lake in the middle, ridged mountains around the rim.
  float TerrainHeight(float x, float z)
  {
    const float r = sqrtf(x * x + z * z);
    const float n = Fbm(x * 0.09f, z * 0.09f, 5) * 2.0f - 1.0f;
    float ridge = 1.0f - fabsf(Fbm(x * 0.045f + 7.0f, z * 0.045f + 3.0f, 4) * 2.0f - 1.0f);
    ridge *= ridge;
    const float rim = Smoothstep(9.0f, 30.0f, r);
    return -3.0f + n * 1.3f + rim * (5.0f + 9.0f * ridge) - 1.6f * expf(-r * r / 40.0f);
  }

  float GroundHeight(float x, float z)
  {
    return std::max(TerrainHeight(x, z), WaterLevel);
  }

  // Cubic arc approximation: handle length 4/3 tan(delta / 4) r.
  void PoseArc(BezierCurve* links, int count, const XMFLOAT3& center, const XMFLOAT3& u, const XMFLOAT3& v,
    float radius, float a0, float a1, bool closed)
  {
    std::vector<XMFLOAT3> points(count + 1);
    std::vector<XMFLOAT3> tangents(count + 1);
    const float delta = (a1 - a0) / float(count);
    for (int k = 0; k <= count; ++k)
    {
      const float a = a0 + delta * float(k);
      points[k] = center + u * (radius * cosf(a)) + v * (radius * sinf(a));
      tangents[k] = u * -sinf(a) + v * cosf(a);
    }
    if (closed)
    {
      points[count] = points[0];
      tangents[count] = tangents[0];
    }

    const float handle = 4.0f / 3.0f * tanf(delta * 0.25f) * radius;
    for (int k = 0; k < count; ++k)
    {
      links[k].control_points(points[k], points[k] + tangents[k] * handle, points[k + 1] - tangents[k + 1] * handle, points[k + 1]);
    }
  }

  BezierStyle MakeStyle(float width, XMFLOAT4 c0, XMFLOAT4 c1, float spacing = 0.0f, float dash = 0.0f,
    CurveCap front = CurveCap::Round, CurveCap back = CurveCap::Round, unsigned resolution = 32u,
    CurveJoin join = CurveJoin::Round)
  {
    BezierStyle style;
    style.width = width;
    style.C0 = c0;
    style.C1 = c1;
    style.spacing = spacing;
    style.dash_length = dash;
    style.cap_front = front;
    style.cap_back = back;
    style.resolution = resolution;
    style.join = join;
    return style;
  }

  // --- scene placement ---------------------------------------------------------------------
  XMFLOAT3 ObeliskBase(int i)
  {
    const float a = (75.0f + 90.0f * float(i)) * Pi / 180.0f;
    const float x = 12.5f * cosf(a);
    const float z = 12.5f * sinf(a);
    return { x, TerrainHeight(x, z) - 0.5f, z };
  }

  constexpr float ObeliskHeight = 6.0f;

  XMFLOAT3 ObeliskTop(int i)
  {
    return ObeliskBase(i) + XMFLOAT3{ 0.0f, ObeliskHeight + 0.45f, 0.0f };
  }

  XMFLOAT3 CoasterPoint(float a)
  {
    const float r = 7.6f + 0.9f * sinf(3.0f * a);
    const float x = r * cosf(a);
    const float z = r * 0.85f * sinf(a);
    const float y = -0.4f + 1.6f * sinf(2.0f * a + 0.5f) + 0.8f * sinf(5.0f * a);
    return { x, std::max(y, GroundHeight(x, z) + 0.7f), z };
  }

  XMFLOAT3 CoasterSide(float a)
  {
    const XMFLOAT3 d = CoasterPoint(a + 0.002f) - CoasterPoint(a - 0.002f);
    return Normalize(XMFLOAT3{ -d.z, 0.0f, d.x });
  }

  const XMFLOAT3 DnaBase = { 10.5f, 0.0f, 6.0f };
  constexpr float DnaHeight = 8.5f;
  constexpr float DnaRadius = 1.1f;
  constexpr float DnaTurns = 2.3f;

  const XMFLOAT3 TorusCenter = { -10.5f, 0.0f, -3.5f };
  constexpr float TorusMajor = 1.8f;
  constexpr float TorusMinor = 0.5f;

  struct MoonInfo
  {
    float radius;
    float speed;
    float phase;
    float pitch;
    float roll;
    float size;
    XMFLOAT4 color;
  };

  const MoonInfo Moons[] = {
    { 5.3f, 0.55f, 0.0f, 0.95f, 0.20f, 0.35f, { 0.55f, 0.85f, 1.00f, 1.0f } },
    { 6.1f, 0.38f, 2.1f, -0.45f, 0.60f, 0.45f, { 1.00f, 0.55f, 0.20f, 1.0f } },
    { 6.9f, 0.27f, 4.0f, 0.30f, -0.75f, 0.30f, { 0.55f, 1.00f, 0.70f, 1.0f } },
  };

  struct MoonFrame
  {
    XMFLOAT3 u;
    XMFLOAT3 v;
  };

  MoonFrame MoonBasis(const MoonInfo& m)
  {
    const XMMATRIX rot = XMMatrixRotationRollPitchYaw(m.pitch, 0.0f, m.roll);
    return { TransformDir({ 1.0f, 0.0f, 0.0f }, rot), TransformDir({ 0.0f, 0.0f, 1.0f }, rot) };
  }

  struct GlossySphere
  {
    float x, z, r;
    XMFLOAT4 color;
    float gloss;
    float spec;
  };

  const GlossySphere Spheres[] = {
    { -9.0f, 8.0f, 1.25f, { 0.85f, 0.86f, 0.90f, 1.0f }, 160.0f, 1.0f },
    { -7.0f, 9.6f, 0.80f, { 1.00f, 0.72f, 0.20f, 1.0f }, 90.0f, 0.8f },
    { -10.8f, 10.0f, 0.70f, { 0.85f, 0.10f, 0.20f, 1.0f }, 60.0f, 0.6f },
    { -6.6f, 7.0f, 0.55f, { 0.10f, 0.75f, 0.45f, 1.0f }, 60.0f, 0.6f },
    { -11.2f, 6.8f, 0.90f, { 0.20f, 0.30f, 0.95f, 1.0f }, 120.0f, 0.7f },
    { -8.2f, 11.0f, 0.45f, { 0.75f, 0.30f, 0.95f, 1.0f }, 60.0f, 0.6f },
  };

  // --- shaders -----------------------------------------------------------------------------
  const char* const ShaderSource = R"HLSL(
cbuffer FrameCB : register(b0)
{
  row_major float4x4 VP;
  row_major float4x4 InvVP;
  float4 Eye;
  float4 SunDir;
  float4 Misc; // x time, y water level
};

cbuffer ObjectCB : register(b1)
{
  row_major float4x4 World;
  float4 Color;
  float4 Color2;
  float4 Params; // x material, y gloss, z specular
};

struct VSIn
{
  float3 pos : POSITION;
  float3 nrm : NORMAL;
};

struct VSOut
{
  float4 pos : SV_Position;
  float3 wpos : TEXCOORD0;
  float3 nrm : TEXCOORD1;
  float3 lpos : TEXCOORD2;
};

float Hash3(float3 p)
{
  p = frac(p * 0.3183099 + 0.1);
  p *= 17.0;
  return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float3 Sky(float3 d)
{
  float3 sun = normalize(SunDir.xyz);
  float up = d.y;
  float3 zenith = float3(0.03, 0.04, 0.14);
  float3 mid = float3(0.22, 0.12, 0.34);
  float3 horizon = float3(1.25, 0.50, 0.28);
  float3 col;
  if (up > 0.0)
    col = lerp(lerp(horizon, mid, saturate(up * 4.0)), zenith, saturate(up * 1.8 - 0.25));
  else
    col = lerp(horizon * 0.55, float3(0.05, 0.04, 0.07), saturate(-up * 5.0));

  float s = saturate(dot(d, sun));
  col += float3(1.0, 0.45, 0.18) * pow(s, 8.0) * 0.7;
  col += float3(1.0, 0.80, 0.55) * pow(s, 300.0) * 1.5;
  col += float3(4.0, 3.2, 2.2) * smoothstep(0.99935, 0.9996, s);

  if (up > 0.04)
  {
    float3 cell = floor(d * 320.0);
    float h = Hash3(cell);
    float twinkle = 0.6 + 0.4 * sin(Misc.x * 3.0 + h * 100.0);
    col += step(0.9965, h) * saturate(up * 3.0 - 0.1) * twinkle * 0.9;
  }
  return col;
}

float3 Tone(float3 c)
{
  return 1.0 - exp(-c * 1.15);
}

VSOut MeshVS(VSIn i)
{
  VSOut o;
  float4 w = mul(float4(i.pos, 1.0), World);
  o.wpos = w.xyz;
  o.nrm = mul(float4(i.nrm, 0.0), World).xyz;
  o.lpos = i.pos;
  o.pos = mul(w, VP);
  return o;
}

float4 MeshPS(VSOut i) : SV_Target
{
  float3 n = normalize(i.nrm);
  float3 V = normalize(Eye.xyz - i.wpos);
  float3 L = normalize(SunDir.xyz);
  uint kind = (uint)Params.x;
  float time = Misc.x;
  float waterLevel = Misc.y;

  float3 albedo = Color.rgb;
  float gloss = Params.y;
  float spec = Params.z;
  float alpha = Color.a;
  float3 emissive = 0;

  if (kind == 0)
  {
    float y = i.wpos.y;
    float slope = 1.0 - n.y;
    float3 sand = float3(0.55, 0.45, 0.32);
    float3 grass = float3(0.14, 0.24, 0.12);
    float3 rock = float3(0.28, 0.24, 0.23);
    float3 snow = float3(0.92, 0.94, 1.0);
    albedo = lerp(sand, grass, smoothstep(waterLevel + 0.15, waterLevel + 0.7, y));
    albedo = lerp(albedo, rock, smoothstep(0.22, 0.42, slope));
    albedo = lerp(albedo, snow, smoothstep(5.5, 7.0, y) * saturate(n.y * 1.6 - 0.4));

    float level = y * 1.25;
    float contour = abs(frac(level + 0.5) - 0.5);
    float contourLine = 1.0 - smoothstep(0.0, fwidth(level) * 1.5, contour);
    emissive += contourLine * float3(0.15, 0.55, 0.85) * 0.35 * step(waterLevel + 0.05, y);
  }
  else if (kind == 2)
  {
    float3 lp = normalize(i.lpos);
    float lat = lp.y;
    float lon = atan2(lp.z, lp.x) + time * 0.08;
    float band = 0.5 + 0.5 * sin(lat * 19.0 + sin(lat * 7.0 + lon * 2.0) * 1.3);
    albedo = lerp(Color.rgb, Color2.rgb, band);
    float dl = lon - 1.0;
    dl = dl - 6.2831853 * round(dl / 6.2831853);
    float spot = exp(-((lat + 0.32) * (lat + 0.32) * 90.0 + dl * dl * 6.0));
    albedo = lerp(albedo, float3(0.85, 0.32, 0.18), spot);
    emissive += float3(0.35, 0.45, 1.0) * pow(1.0 - saturate(dot(n, V)), 3.0) * 0.7;
  }
  else if (kind == 3)
  {
    float2 p = i.wpos.xz;
    n = normalize(float3(
      sin(p.x * 1.3 + time * 1.2) * 0.035 + sin(p.y * 2.7 - time * 1.7) * 0.02,
      1.0,
      cos(p.y * 1.7 + time * 0.9) * 0.035 + cos(p.x * 2.3 + time * 1.4) * 0.02));
  }
  else if (kind == 4)
  {
    emissive += Color.rgb * 1.4;
  }

  float ndl = saturate(dot(n, L));
  float3 H = normalize(L + V);
  float3 sunColor = float3(1.0, 0.72, 0.50) * 1.2;
  float3 fillDir = normalize(float3(-0.3, 0.8, 0.5));
  float3 fillColor = float3(0.20, 0.24, 0.42);
  float3 ambient = lerp(float3(0.08, 0.05, 0.09), float3(0.10, 0.12, 0.26), n.y * 0.5 + 0.5);
  float fres = pow(1.0 - saturate(dot(n, V)), 5.0);
  float3 refl = Sky(reflect(-V, n));

  float3 col = albedo * (ambient + sunColor * ndl + fillColor * saturate(dot(n, fillDir)));
  col += sunColor * spec * pow(saturate(dot(n, H)), gloss) * step(0.0, dot(n, L));
  col += refl * lerp(0.04, 1.0, fres) * spec;
  col += emissive;

  if (kind == 3)
  {
    alpha = lerp(0.72, 0.97, fres);
  }

  float dist = length(Eye.xyz - i.wpos);
  float fog = 1.0 - exp(-dist * dist * 0.00018);
  float3 fogColor = Sky(normalize(float3(-V.x, 0.03, -V.z)));
  col = lerp(col, fogColor, saturate(fog) * 0.85);

  return float4(Tone(col), alpha);
}

struct SkyOut
{
  float4 pos : SV_Position;
  float2 ndc : TEXCOORD0;
};

SkyOut SkyVS(uint id : SV_VertexID)
{
  SkyOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  o.ndc = o.pos.xy;
  return o;
}

float4 SkyPS(SkyOut i) : SV_Target
{
  float4 p = mul(float4(i.ndc, 1.0, 1.0), InvVP);
  float3 d = normalize(p.xyz / p.w - Eye.xyz);
  return float4(Tone(Sky(d)), 1.0);
}
)HLSL";

  winrt::com_ptr<ID3DBlob> CompileShader(const char* entry, const char* target)
  {
    winrt::com_ptr<ID3DBlob> code;
    winrt::com_ptr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(ShaderSource, strlen(ShaderSource), "ShowcaseShaders", nullptr, nullptr,
      entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
    if (FAILED(hr))
    {
      const std::string message = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "unknown error";
      OutputDebugStringA(message.c_str());
      std::cerr << message << std::endl;
      throw std::runtime_error(message);
    }
    return code;
  }

  struct MeshVertex
  {
    XMFLOAT3 position;
    XMFLOAT3 normal;
  };

  struct FrameConstants
  {
    XMFLOAT4X4 VP;
    XMFLOAT4X4 InvVP;
    XMFLOAT4 Eye;
    XMFLOAT4 SunDir;
    XMFLOAT4 Misc;
  };

  struct ObjectConstants
  {
    XMFLOAT4X4 World;
    XMFLOAT4 Color;
    XMFLOAT4 Color2;
    XMFLOAT4 Params;
  };

  void BuildSphere(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, int stacks, int slices)
  {
    for (int i = 0; i <= stacks; ++i)
    {
      const float phi = Pi * float(i) / float(stacks);
      for (int j = 0; j <= slices; ++j)
      {
        const float theta = Tau * float(j) / float(slices);
        const XMFLOAT3 p = { sinf(phi) * cosf(theta), cosf(phi), sinf(phi) * sinf(theta) };
        vertices.push_back({ p, p });
      }
    }
    for (int i = 0; i < stacks; ++i)
    {
      for (int j = 0; j < slices; ++j)
      {
        const uint32_t a = uint32_t(i * (slices + 1) + j);
        const uint32_t b = a + uint32_t(slices + 1);
        indices.insert(indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
      }
    }
  }

  void BuildTorus(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, float major, float minor, int rings, int sides)
  {
    for (int i = 0; i <= rings; ++i)
    {
      const float u = Tau * float(i) / float(rings);
      for (int j = 0; j <= sides; ++j)
      {
        const float v = Tau * float(j) / float(sides);
        const XMFLOAT3 n = { cosf(v) * cosf(u), sinf(v), cosf(v) * sinf(u) };
        const XMFLOAT3 p = { (major + minor * cosf(v)) * cosf(u), minor * sinf(v), (major + minor * cosf(v)) * sinf(u) };
        vertices.push_back({ p, n });
      }
    }
    for (int i = 0; i < rings; ++i)
    {
      for (int j = 0; j < sides; ++j)
      {
        const uint32_t a = uint32_t(i * (sides + 1) + j);
        const uint32_t b = a + uint32_t(sides + 1);
        indices.insert(indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
      }
    }
  }

  void BuildBox(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)
  {
    const XMFLOAT3 normals[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const auto& n : normals)
    {
      const XMFLOAT3 t = fabsf(n.y) > 0.5f ? XMFLOAT3{ 1, 0, 0 } : XMFLOAT3{ 0, 1, 0 };
      const XMFLOAT3 b = { n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x };
      const uint32_t base = uint32_t(vertices.size());
      for (int k = 0; k < 4; ++k)
      {
        const float su = (k & 1) ? 0.5f : -0.5f;
        const float sv = (k & 2) ? 0.5f : -0.5f;
        vertices.push_back({ n * 0.5f + t * su + b * sv, n });
      }
      indices.insert(indices.end(), { base, base + 1, base + 2, base + 2, base + 1, base + 3 });
    }
  }

  void BuildTerrain(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices)
  {
    const int n = TerrainCells;
    const float step = 2.0f * TerrainExtent / float(n);
    const float e = step * 0.5f;
    for (int i = 0; i <= n; ++i)
    {
      for (int j = 0; j <= n; ++j)
      {
        const float x = -TerrainExtent + step * float(j);
        const float z = -TerrainExtent + step * float(i);
        const float dx = TerrainHeight(x + e, z) - TerrainHeight(x - e, z);
        const float dz = TerrainHeight(x, z + e) - TerrainHeight(x, z - e);
        vertices.push_back({ { x, TerrainHeight(x, z), z }, Normalize(XMFLOAT3{ -dx, 2.0f * e, -dz }) });
      }
    }
    for (int i = 0; i < n; ++i)
    {
      for (int j = 0; j < n; ++j)
      {
        const uint32_t a = uint32_t(i * (n + 1) + j);
        const uint32_t b = a + uint32_t(n + 1);
        indices.insert(indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
      }
    }
  }
}

// --- mesh pass ---------------------------------------------------------------------------------

void App::CreateMeshPass()
{
  auto* d3d = device->get();

  auto upload = [&](GpuMesh& mesh, const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>& indices)
  {
    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.ByteWidth = UINT(vertices.size() * sizeof(MeshVertex));
    D3D11_SUBRESOURCE_DATA data = { vertices.data() };
    winrt::check_hresult(d3d->CreateBuffer(&desc, &data, mesh.vertices.put()));

    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    desc.ByteWidth = UINT(indices.size() * sizeof(uint32_t));
    data.pSysMem = indices.data();
    winrt::check_hresult(d3d->CreateBuffer(&desc, &data, mesh.indices.put()));
    mesh.index_count = UINT(indices.size());
  };

  {
    std::vector<MeshVertex> v;
    std::vector<uint32_t> i;
    BuildTerrain(v, i);
    upload(terrain_mesh, v, i);
  }
  {
    const float s = TerrainExtent;
    std::vector<MeshVertex> v = {
      { { -s, WaterLevel, -s }, { 0, 1, 0 } },
      { { s, WaterLevel, -s }, { 0, 1, 0 } },
      { { -s, WaterLevel, s }, { 0, 1, 0 } },
      { { s, WaterLevel, s }, { 0, 1, 0 } },
    };
    std::vector<uint32_t> i = { 0, 2, 1, 1, 2, 3 };
    upload(water_mesh, v, i);
  }
  {
    std::vector<MeshVertex> v;
    std::vector<uint32_t> i;
    BuildSphere(v, i, 48, 96);
    upload(sphere_mesh, v, i);
  }
  {
    std::vector<MeshVertex> v;
    std::vector<uint32_t> i;
    BuildTorus(v, i, TorusMajor, TorusMinor, 128, 40);
    upload(torus_mesh, v, i);
  }
  {
    std::vector<MeshVertex> v;
    std::vector<uint32_t> i;
    BuildBox(v, i);
    upload(box_mesh, v, i);
  }

  auto vs_code = CompileShader("MeshVS", "vs_5_0");
  auto ps_code = CompileShader("MeshPS", "ps_5_0");
  auto sky_vs_code = CompileShader("SkyVS", "vs_5_0");
  auto sky_ps_code = CompileShader("SkyPS", "ps_5_0");

  winrt::check_hresult(d3d->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, mesh_vs.put()));
  winrt::check_hresult(d3d->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, mesh_ps.put()));
  winrt::check_hresult(d3d->CreateVertexShader(sky_vs_code->GetBufferPointer(), sky_vs_code->GetBufferSize(), nullptr, sky_vs.put()));
  winrt::check_hresult(d3d->CreatePixelShader(sky_ps_code->GetBufferPointer(), sky_ps_code->GetBufferSize(), nullptr, sky_ps.put()));

  const D3D11_INPUT_ELEMENT_DESC layout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
  };
  winrt::check_hresult(d3d->CreateInputLayout(layout, 2, vs_code->GetBufferPointer(), vs_code->GetBufferSize(), mesh_layout.put()));

  {
    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.ByteWidth = sizeof(FrameConstants);
    winrt::check_hresult(d3d->CreateBuffer(&desc, nullptr, frame_constants.put()));
    desc.ByteWidth = sizeof(ObjectConstants);
    winrt::check_hresult(d3d->CreateBuffer(&desc, nullptr, object_constants.put()));
  }

  {
    D3D11_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D11_FILL_SOLID;
    desc.CullMode = D3D11_CULL_NONE;
    desc.DepthClipEnable = TRUE;
    winrt::check_hresult(d3d->CreateRasterizerState(&desc, raster_state.put()));
  }

  {
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = FALSE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    winrt::check_hresult(d3d->CreateDepthStencilState(&desc, depth_off.put()));
    desc.DepthEnable = TRUE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    desc.DepthFunc = D3D11_COMPARISON_GREATER;
    winrt::check_hresult(d3d->CreateDepthStencilState(&desc, depth_on.put()));
  }

  {
    D3D11_BLEND_DESC desc = {};
    desc.RenderTarget[0].BlendEnable = FALSE;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    winrt::check_hresult(d3d->CreateBlendState(&desc, blend_opaque.put()));

    auto& rt = desc.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    winrt::check_hresult(d3d->CreateBlendState(&desc, blend_alpha.put()));
  }
}

void App::DrawMesh(const GpuMesh& mesh, const XMMATRIX& world, const XMFLOAT4& color, const XMFLOAT4& color2, const XMFLOAT4& params)
{
  auto* ctx = device->ImmediateContext()->get();

  ObjectConstants constants;
  XMStoreFloat4x4(&constants.World, world);
  constants.Color = color;
  constants.Color2 = color2;
  constants.Params = params;
  ctx->UpdateSubresource(object_constants.get(), 0, nullptr, &constants, 0, 0);

  ID3D11Buffer* vb = mesh.vertices.get();
  const UINT stride = sizeof(MeshVertex);
  const UINT offset = 0;
  ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  ctx->IASetIndexBuffer(mesh.indices.get(), DXGI_FORMAT_R32_UINT, 0);
  ctx->DrawIndexed(mesh.index_count, 0, 0);
}

void App::DrawEnvironment()
{
  auto* ctx = device->ImmediateContext()->get();
  const float t = animation_time;

  {
    FrameConstants frame;
    const XMMATRIX view_proj = camera.GetViewProj();
    XMVECTOR det;
    XMStoreFloat4x4(&frame.VP, view_proj);
    XMStoreFloat4x4(&frame.InvVP, XMMatrixInverse(&det, view_proj));
    XMStoreFloat4(&frame.Eye, camera.GetEye());
    const XMFLOAT3 sun = Normalize(SunDirection);
    frame.SunDir = { sun.x, sun.y, sun.z, 0.0f };
    frame.Misc = { t, WaterLevel, 0.0f, 0.0f };
    ctx->UpdateSubresource(frame_constants.get(), 0, nullptr, &frame, 0, 0);
  }

  ID3D11Buffer* cbs[] = { frame_constants.get(), object_constants.get() };
  ctx->VSSetConstantBuffers(0, 2, cbs);
  ctx->PSSetConstantBuffers(0, 2, cbs);
  ctx->HSSetShader(nullptr, nullptr, 0);
  ctx->DSSetShader(nullptr, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->RSSetState(raster_state.get());
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  const float blend_factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

  ctx->OMSetDepthStencilState(depth_off.get(), 0);
  ctx->OMSetBlendState(blend_opaque.get(), blend_factor, 0xFFFFFFFFu);
  ctx->IASetInputLayout(nullptr);
  ctx->VSSetShader(sky_vs.get(), nullptr, 0);
  ctx->PSSetShader(sky_ps.get(), nullptr, 0);
  ctx->Draw(3, 0);

  if (draw_meshes)
  {
    ctx->OMSetDepthStencilState(depth_on.get(), 0);
    ctx->IASetInputLayout(mesh_layout.get());
    ctx->VSSetShader(mesh_vs.get(), nullptr, 0);
    ctx->PSSetShader(mesh_ps.get(), nullptr, 0);

    const XMFLOAT4 none = { 0, 0, 0, 0 };

    DrawMesh(terrain_mesh, XMMatrixIdentity(), { 1, 1, 1, 1 }, none, { 0, 1, 0, 0 });

    DrawMesh(sphere_mesh,
      XMMatrixScaling(PlanetRadius, PlanetRadius, PlanetRadius) * XMMatrixRotationZ(0.3f) * XMMatrixTranslation(PlanetCenter.x, PlanetCenter.y, PlanetCenter.z),
      { 0.92f, 0.72f, 0.48f, 1.0f }, { 0.55f, 0.30f, 0.30f, 1.0f }, { 2, 40, 0.25f, 0 });

    for (const auto& m : Moons)
    {
      const MoonFrame basis = MoonBasis(m);
      const float a = t * m.speed + m.phase;
      const XMFLOAT3 p = PlanetCenter + basis.u * (m.radius * cosf(a)) + basis.v * (m.radius * sinf(a));
      DrawMesh(sphere_mesh, XMMatrixScaling(m.size, m.size, m.size) * XMMatrixTranslation(p.x, p.y, p.z),
        Scale3(m.color, 0.6f), none, { 4, 60, 0.5f, 0 });
    }

    for (const auto& s : Spheres)
    {
      const float y = GroundHeight(s.x, s.z) + s.r * 0.85f;
      DrawMesh(sphere_mesh, XMMatrixScaling(s.r, s.r, s.r) * XMMatrixTranslation(s.x, y, s.z), s.color, none, { 1, s.gloss, s.spec, 0 });
    }

    {
      const float cy = GroundHeight(TorusCenter.x, TorusCenter.z) + 3.6f;
      const XMMATRIX world = XMMatrixRotationRollPitchYaw(0.9f + 0.25f * sinf(t * 0.3f), t * 0.35f, 0.35f) * XMMatrixTranslation(TorusCenter.x, cy, TorusCenter.z);
      DrawMesh(torus_mesh, world, { 0.12f, 0.12f, 0.16f, 1.0f }, none, { 1, 120, 1.0f, 0 });
    }

    for (int i = 0; i < ObeliskCount; ++i)
    {
      const XMFLOAT3 b = ObeliskBase(i);
      DrawMesh(box_mesh, XMMatrixScaling(0.7f, ObeliskHeight, 0.7f) * XMMatrixRotationY(float(i) * 0.7f) * XMMatrixTranslation(b.x, b.y + ObeliskHeight * 0.5f, b.z),
        { 0.06f, 0.06f, 0.08f, 1.0f }, none, { 1, 80, 0.8f, 0 });
      const XMFLOAT3 top = ObeliskTop(i);
      DrawMesh(sphere_mesh, XMMatrixScaling(0.32f, 0.32f, 0.32f) * XMMatrixTranslation(top.x, top.y, top.z),
        Scale3(HueColor(float(i) / ObeliskCount), 0.8f), none, { 4, 60, 0.3f, 0 });
    }

    for (int k = 0; k < CartCount; ++k)
    {
      const float a = t * 0.35f + float(k) * 0.075f;
      const XMFLOAT3 p = CoasterPoint(a) + XMFLOAT3{ 0.0f, 0.28f, 0.0f };
      DrawMesh(sphere_mesh, XMMatrixScaling(0.26f, 0.26f, 0.26f) * XMMatrixTranslation(p.x, p.y, p.z),
        Scale3(HueColor(0.05f + 0.08f * float(k)), 0.7f), none, { 4, 60, 0.5f, 0 });
    }

    ctx->OMSetBlendState(blend_alpha.get(), blend_factor, 0xFFFFFFFFu);
    DrawMesh(water_mesh, XMMatrixIdentity(), { 0.01f, 0.05f, 0.08f, 1.0f }, none, { 3, 300, 1.0f, 0 });
  }

  ctx->OMSetBlendState(nullptr, blend_factor, 0xFFFFFFFFu);
  ctx->IASetInputLayout(nullptr);
}

// --- curves ------------------------------------------------------------------------------------

BezierCurve App::AddCurve(const BezierStyle& style, bool merge)
{
  BezierData data;
  data.style = style;
  data.merge_with_previous = merge;
  return curves->Add(data);
}

void App::BuildScene()
{
  orbits.reserve(MoonCount);
  wind.reserve(WindCount);
  aurora.reserve(AuroraCount);

  // Planet rings: thin translucent solid circles, a gap for the division.
  ring_links.reserve(RingCount * RingLinks);
  for (int r = 0; r < RingCount; ++r)
  {
    const float width = 2.0f + 3.0f * Hash1(r * 7 + 1);
    const BezierStyle style = MakeStyle(width, { 1, 1, 1, 1 }, { 1, 1, 1, 1 });
    for (int k = 0; k < RingLinks; ++k)
    {
      ring_links.push_back(AddCurve(style, k > 0));
    }
  }

  // Moon orbits as dotted closed Hermite loops, plus a solid fading trail behind each moon.
  for (int m = 0; m < MoonCount; ++m)
  {
    BezierStyle style = MakeStyle(4.0f, WithAlpha(Moons[m].color, 0.55f), WithAlpha(Moons[m].color, 0.55f), 0.22f, 0.0f);
    orbits.emplace_back(*curves, style, OrbitLinks + 1);
  }
  for (int m = 0; m < MoonCount; ++m)
  {
    const BezierStyle style = MakeStyle(7.0f, { 1, 1, 1, 0 }, { 1, 1, 1, 1 }, 0.0f, 0.0f, CurveCap::Butt, CurveCap::Round);
    for (int k = 0; k < TrailLinks; ++k)
    {
      trail_links.push_back(AddCurve(style, k > 0));
    }
  }

  // DNA double helix: two Catmull-Rom strands and linear rungs.
  {
    const BezierStyle strand = MakeStyle(13.0f, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0.0f, 0.0f, CurveCap::Round, CurveCap::Round, 24u);
    dna_a = CatmullRomSpline(*curves, strand, DnaPoints);
    dna_b = CatmullRomSpline(*curves, strand, DnaPoints);
    const BezierStyle rung = MakeStyle(5.0f, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0.0f, 0.0f, CurveCap::Round, CurveCap::Round, 2u);
    for (int i = 0; i < DnaRungs; ++i)
    {
      BezierData data = Linear({ 0, 0, 0 }, { 0, 0, 0 });
      data.style = rung;
      dna_rungs.push_back(curves->Add(data));
    }
  }

  // Dashed (2, 7) torus knot around the torus.
  {
    const BezierStyle style = MakeStyle(7.0f, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0.20f, 14.0f, CurveCap::Round, CurveCap::Round, 12u);
    knot = CatmullRomSpline(*curves, style, KnotPoints, 0.5f);
  }

  // Chevron-dashed cubic routes between the obelisk tips: notch at the front, point at the back.
  for (int i = 0; i < ObeliskCount; ++i)
  {
    for (int j = i + 1; j < ObeliskCount; ++j)
    {
      const BezierStyle style = MakeStyle(10.0f, HueColor(float(i) / ObeliskCount), HueColor(float(j) / ObeliskCount),
        0.75f, 18.0f, CurveCap::TriangleIn, CurveCap::TriangleOut, 64u);
      routes.push_back(AddCurve(style, false));
    }
  }

  // Wind streamlines.
  for (int w = 0; w < WindCount; ++w)
  {
    const BezierStyle style = MakeStyle(3.5f, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0.5f, 26.0f, CurveCap::Round, CurveCap::Round, 20u);
    wind.emplace_back(*curves, style, WindPoints, 0.5f);
  }

  // Roller coaster: height-coloured rails and dashed sleepers.
  {
    BezierStyle rail = MakeStyle(5.0f, { 0.10f, 0.75f, 1.0f, 1.0f }, { 1.0f, 0.20f, 0.45f, 1.0f }, 0.0f, 0.0f,
      CurveCap::Round, CurveCap::Round, 24u, CurveJoin::Round);
    rail.min_height = -2.2f;
    rail.max_height = 2.4f;
    rail_left = CatmullRomSpline(*curves, rail, CoasterPoints);
    rail_right = CatmullRomSpline(*curves, rail, CoasterPoints);

    const BezierStyle ties = MakeStyle(20.0f, { 0.30f, 0.20f, 0.14f, 1.0f }, { 0.30f, 0.20f, 0.14f, 1.0f }, 0.28f, 3.0f,
      CurveCap::Butt, CurveCap::Butt, 24u);
    sleepers = CatmullRomSpline(*curves, ties, CoasterPoints);
  }

  // Aurora: wide, translucent, overlapping ribbons.
  for (int k = 0; k < AuroraCount; ++k)
  {
    const BezierStyle style = MakeStyle(127.5f, { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, 0.0f, 0.0f, CurveCap::Butt, CurveCap::Butt, 24u);
    aurora.emplace_back(*curves, style, AuroraPoints, 0.5f);
  }

  // Fireworks: one burst per backend.
  for (int b = 0; b < BurstCount; ++b)
  {
    BezierStyle style;
    switch (b)
    {
    case 0: style = MakeStyle(4.0f, { 1, 1, 1, 0 }, { 1, 1, 1, 1 }, 0.0f, 0.0f, CurveCap::Butt, CurveCap::Round, 16u); break;
    case 1: style = MakeStyle(5.0f, { 1, 1, 1, 0 }, { 1, 1, 1, 1 }, 0.14f, 0.0f, CurveCap::Round, CurveCap::Round, 16u); break;
    default: style = MakeStyle(4.0f, { 1, 1, 1, 0 }, { 1, 1, 1, 1 }, 0.28f, 9.0f, CurveCap::Round, CurveCap::TriangleOut, 16u); break;
    }
    for (int s = 0; s < SparkCount; ++s)
    {
      BezierData data = Quadratic({ 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 });
      data.style = style;
      sparks.push_back(curves->Add(data));
    }
  }

  // Ripples on the lake under the planet, alternating solid and dotted.
  for (int r = 0; r < RippleCount; ++r)
  {
    const BezierStyle style = (r & 1)
      ? MakeStyle(4.0f, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0.16f, 0.0f)
      : MakeStyle(3.0f, { 1, 1, 1, 1 }, { 1, 1, 1, 1 });
    for (int k = 0; k < RippleLinks; ++k)
    {
      ripple_links.push_back(AddCurve(style, k > 0));
    }
  }
}

void App::PoseScene()
{
  const float t = animation_time;
  const XMFLOAT3 sun = Normalize(SunDirection);

  // --- planet rings --------------------------------------------------------------------------
  {
    const XMMATRIX rot = XMMatrixRotationRollPitchYaw(0.42f, 0.0f, 0.30f);
    const XMFLOAT3 u = TransformDir({ 1, 0, 0 }, rot);
    const XMFLOAT3 v = TransformDir({ 0, 0, 1 }, rot);
    for (int r = 0; r < RingCount; ++r)
    {
      float s = float(r) / float(RingCount - 1);
      float radius = 2.7f + 2.3f * s + (r >= 11 ? 0.35f : 0.0f);
      BezierCurve* links = &ring_links[size_t(r * RingLinks)];
      const float spin = t * (0.08f / (0.5f + s));
      PoseArc(links, RingLinks, PlanetCenter, u, v, radius, spin, spin + Tau, true);

      const XMFLOAT4 inner = { 0.95f, 0.82f, 0.62f, 1.0f };
      const XMFLOAT4 outer = { 0.62f, 0.55f, 0.85f, 1.0f };
      const float alpha = 0.25f + 0.45f * Hash1(r * 13 + 5);
      auto ring_color = [&](float a)
      {
        const XMFLOAT3 p = u * (radius * cosf(a)) + v * (radius * sinf(a));
        const float along = Dot(p, sun);
        const float perp = Length(p - sun * along);
        const float shadow = along < 0.0f ? Smoothstep(PlanetRadius * 0.85f, PlanetRadius * 1.1f, perp) : 1.0f;
        const float lit = (0.55f + 0.45f * std::max(0.0f, along / radius)) * (0.25f + 0.75f * shadow);
        XMFLOAT4 c = {
          (inner.x + (outer.x - inner.x) * s) * lit,
          (inner.y + (outer.y - inner.y) * s) * lit,
          (inner.z + (outer.z - inner.z) * s) * lit,
          alpha
        };
        return c;
      };
      for (int k = 0; k < RingLinks; ++k)
      {
        const float a0 = spin + Tau * float(k) / RingLinks;
        const float a1 = spin + Tau * float(k + 1) / RingLinks;
        links[k].colors(ring_color(a0), ring_color(a1));
      }
    }
  }

  // --- moon orbits and trails ----------------------------------------------------------------
  for (int m = 0; m < MoonCount; ++m)
  {
    const MoonInfo& info = Moons[m];
    const MoonFrame basis = MoonBasis(info);

    auto& orbit = orbits[size_t(m)];
    const float delta = Tau / OrbitLinks;
    const float tangent = 4.0f * tanf(delta * 0.25f) * info.radius;
    for (int k = 0; k < OrbitLinks; ++k)
    {
      const float a = delta * float(k);
      orbit[size_t(k)].position = PlanetCenter + basis.u * (info.radius * cosf(a)) + basis.v * (info.radius * sinf(a));
      orbit[size_t(k)].tangent = (basis.u * -sinf(a) + basis.v * cosf(a)) * tangent;
    }
    orbit[OrbitLinks] = orbit[0];
    orbit.Update();

    const float head = t * info.speed + info.phase;
    const float tail = head - 2.4f;
    BezierCurve* links = &trail_links[size_t(m * TrailLinks)];
    PoseArc(links, TrailLinks, PlanetCenter, basis.u, basis.v, info.radius, tail, head, false);
    for (int k = 0; k < TrailLinks; ++k)
    {
      const float f0 = float(k) / TrailLinks;
      const float f1 = float(k + 1) / TrailLinks;
      links[k].colors(WithAlpha(info.color, f0 * f0 * 0.95f), WithAlpha(info.color, f1 * f1 * 0.95f));
    }
  }

  // --- DNA -----------------------------------------------------------------------------------
  {
    const float base_y = GroundHeight(DnaBase.x, DnaBase.z) - 0.2f;
    const float spin = t * 0.6f;
    auto helix = [&](float s, float offset)
    {
      const float a = s * DnaTurns * Tau + spin + offset;
      return XMFLOAT3{ DnaBase.x + DnaRadius * cosf(a), base_y + s * DnaHeight, DnaBase.z + DnaRadius * sinf(a) };
    };

    XMFLOAT4 colors_a[DnaPoints];
    XMFLOAT4 colors_b[DnaPoints];
    for (int k = 0; k < DnaPoints; ++k)
    {
      const float s = float(k) / float(DnaPoints - 1);
      dna_a[size_t(k)] = helix(s, 0.0f);
      dna_b[size_t(k)] = helix(s, Pi);
      colors_a[k] = HueColor(0.95f + 0.25f * s + t * 0.05f);
      colors_b[k] = HueColor(0.45f + 0.25f * s + t * 0.05f);
    }
    dna_a.MirrorEnds();
    dna_b.MirrorEnds();
    dna_a.Update();
    dna_b.Update();
    dna_a.PointColors(colors_a);
    dna_b.PointColors(colors_b);

    for (int i = 0; i < DnaRungs; ++i)
    {
      const float s = (float(i) + 0.5f) / float(DnaRungs);
      const XMFLOAT3 a = helix(s, 0.0f);
      const XMFLOAT3 b = helix(s, Pi);
      const XMFLOAT3 inset = (b - a) * 0.08f;
      dna_rungs[size_t(i)].control_points(a + inset, b - inset);
      dna_rungs[size_t(i)].colors(HueColor(0.95f + 0.25f * s + t * 0.05f, 0.9f), HueColor(0.45f + 0.25f * s + t * 0.05f, 0.9f));
    }
  }

  // --- torus knot ----------------------------------------------------------------------------
  {
    const float cy = GroundHeight(TorusCenter.x, TorusCenter.z) + 3.6f;
    const XMMATRIX world = XMMatrixRotationRollPitchYaw(0.9f + 0.25f * sinf(t * 0.3f), t * 0.35f, 0.35f) * XMMatrixTranslation(TorusCenter.x, cy, TorusCenter.z);
    const float tube = TorusMinor * 1.45f;
    auto knot_point = [&](float s)
    {
      const float phi = s * Tau;
      const float u = 2.0f * phi;
      const float v = 7.0f * phi;
      const XMFLOAT3 local = { (TorusMajor + tube * cosf(v)) * cosf(u), tube * sinf(v), (TorusMajor + tube * cosf(v)) * sinf(u) };
      return Transform(local, world);
    };

    const float step = 1.0f / float(KnotPoints - 1);
    std::vector<XMFLOAT4> colors(KnotPoints);
    for (int k = 0; k < KnotPoints; ++k)
    {
      knot[size_t(k)] = knot_point(float(k) * step);
      colors[size_t(k)] = HueColor(2.0f * float(k) * step - t * 0.15f);
    }
    knot[KnotPoints - 1] = knot[0];
    colors[KnotPoints - 1] = colors[0];
    knot.FrontEnd() = knot[KnotPoints - 2];
    knot.BackEnd() = knot[1];
    knot.Update();
    knot.PointColors(colors);
  }

  // --- obelisk routes ------------------------------------------------------------------------
  {
    size_t index = 0;
    for (int i = 0; i < ObeliskCount; ++i)
    {
      for (int j = i + 1; j < ObeliskCount; ++j)
      {
        const XMFLOAT3 a = ObeliskTop(i);
        const XMFLOAT3 d = ObeliskTop(j);
        const XMFLOAT3 span = d - a;
        const float lift = 0.35f * Length(span) + 2.0f + 0.5f * sinf(t * 0.8f + float(index));
        const XMFLOAT3 up = { 0.0f, lift, 0.0f };
        routes[index].control_points(a, a + up + span * 0.15f, d + up - span * 0.15f, d);
        ++index;
      }
    }
  }

  // --- wind ----------------------------------------------------------------------------------
  for (int w = 0; w < WindCount; ++w)
  {
    const float lane = -13.0f + 26.0f * (float(w) + 0.5f) / WindCount + (Hash1(w * 3) - 0.5f) * 0.6f;
    const float length = 6.0f + 4.0f * Hash1(w * 5 + 1);
    const float speed = 0.06f + 0.05f * Hash1(w * 11 + 2);
    const float phase = Fract(t * speed + Hash1(w * 17 + 3));
    const float span = 34.0f + length;
    const float x0 = -17.0f - length + phase * span;
    const float envelope = sinf(Pi * phase);

    auto& line = wind[size_t(w)];
    XMFLOAT4 colors[WindPoints];
    for (int k = 0; k < WindPoints; ++k)
    {
      const float s = float(k) / float(WindPoints - 1);
      const float x = x0 + length * s;
      const float z = lane + 0.8f * sinf(0.35f * x + float(w));
      const float y = GroundHeight(x, z) + 0.55f + 0.15f * sinf(x * 0.8f + t * 2.0f + float(w));
      line[size_t(k)] = { x, y, z };
      colors[k] = { 0.85f, 0.95f, 1.0f, envelope * sinf(Pi * s) * 0.85f };
    }
    line.MirrorEnds();
    line.Update();
    line.PointColors(colors);
  }

  // --- roller coaster ------------------------------------------------------------------------
  {
    const float step = Tau / float(CoasterPoints - 1);
    auto pose = [&](CatmullRomSpline& spline, float side, float lift)
    {
      auto at = [&](float a) { return CoasterPoint(a) + CoasterSide(a) * side + XMFLOAT3{ 0.0f, lift, 0.0f }; };
      for (int k = 0; k < CoasterPoints; ++k)
      {
        spline[size_t(k)] = at(float(k) * step);
      }
      spline[CoasterPoints - 1] = spline[0];
      spline.FrontEnd() = at(-step);
      spline.BackEnd() = at(step);
      spline.Update();
    };
    pose(rail_left, 0.2f, 0.0f);
    pose(rail_right, -0.2f, 0.0f);
    pose(sleepers, 0.0f, -0.04f);
  }

  // --- aurora --------------------------------------------------------------------------------
  {
    const XMFLOAT4 palette[AuroraCount] = {
      { 0.25f, 1.00f, 0.55f, 1.0f },
      { 0.15f, 0.85f, 0.95f, 1.0f },
      { 0.60f, 0.35f, 1.00f, 1.0f },
      { 0.30f, 1.00f, 0.40f, 1.0f },
      { 0.95f, 0.30f, 0.75f, 1.0f },
    };
    for (int k = 0; k < AuroraCount; ++k)
    {
      auto& ribbon = aurora[size_t(k)];
      XMFLOAT4 colors[AuroraPoints];
      for (int i = 0; i < AuroraPoints; ++i)
      {
        const float s = float(i) / float(AuroraPoints - 1);
        const float x = -60.0f + 120.0f * s;
        const float y = 12.5f + 0.7f * float(k) + 1.6f * sinf(x * 0.07f + t * 0.25f + float(k) * 1.3f);
        const float z = -44.0f - 4.0f * float(k) + 3.0f * sinf(x * 0.05f + t * 0.2f + float(k));
        ribbon[size_t(i)] = { x, y, z };
        const float pulse = 0.55f + 0.45f * sinf(x * 0.15f + t * 0.7f + float(k) * 2.0f);
        colors[i] = WithAlpha(palette[k], 0.20f * sinf(Pi * s) * pulse);
      }
      ribbon.MirrorEnds();
      ribbon.Update();
      ribbon.PointColors(colors);
    }
  }

  // --- fireworks -----------------------------------------------------------------------------
  {
    const XMFLOAT3 centers[BurstCount] = { { -15.0f, 9.5f, -22.0f }, { 1.0f, 10.5f, -27.0f }, { 14.0f, 9.0f, -20.0f } };
    constexpr float Period = 3.2f;
    for (int b = 0; b < BurstCount; ++b)
    {
      const float clock = t / Period + float(b) * 0.37f;
      const int cycle = int(floorf(clock));
      const float p = clock - float(cycle);
      const float radius = 3.2f * (1.0f - (1.0f - p) * (1.0f - p));
      const float fade = 1.0f - p * p;
      const XMFLOAT4 hue = HueColor(Hash(cycle, b));
      const XMFLOAT3 c = centers[b];

      for (int s = 0; s < SparkCount; ++s)
      {
        const float fy = 1.0f - 2.0f * (float(s) + 0.5f) / SparkCount;
        const float fr = sqrtf(std::max(0.0f, 1.0f - fy * fy));
        const float fa = float(s) * 2.39996f + float(cycle);
        const XMFLOAT3 dir = { fr * cosf(fa), fy, fr * sinf(fa) };
        const float tail_r = radius * std::max(0.0f, (p - 0.3f) / 0.7f);
        auto droop = [&](float amount) { return XMFLOAT3{ 0.0f, -1.8f * amount * amount, 0.0f }; };
        const XMFLOAT3 head = c + dir * radius + droop(p);
        const XMFLOAT3 tailp = c + dir * tail_r + droop(std::max(0.0f, p - 0.25f));
        const XMFLOAT3 mid = c + dir * (0.5f * (radius + tail_r)) + droop(std::max(0.0f, p - 0.12f)) * 0.8f;

        BezierCurve& spark = sparks[size_t(b * SparkCount + s)];
        spark.control_points(tailp, mid, head);
        spark.colors(WithAlpha(hue, 0.0f), { 0.5f + 0.5f * hue.x, 0.5f + 0.5f * hue.y, 0.5f + 0.5f * hue.z, fade });
      }
    }
  }

  // --- ripples -------------------------------------------------------------------------------
  {
    const XMFLOAT3 center = { PlanetCenter.x, WaterLevel + 0.04f, PlanetCenter.z };
    for (int r = 0; r < RippleCount; ++r)
    {
      const float phase = Fract(t * 0.1f + float(r) / RippleCount);
      const float radius = 0.6f + 6.5f * phase;
      const float alpha = powf(1.0f - phase, 1.5f) * 0.8f;
      BezierCurve* links = &ripple_links[size_t(r * RippleLinks)];
      PoseArc(links, RippleLinks, center, { 1, 0, 0 }, { 0, 0, 1 }, radius, 0.0f, Tau, true);
      const XMFLOAT4 color = (r & 1) ? XMFLOAT4{ 1.0f, 1.0f, 1.0f, alpha } : XMFLOAT4{ 0.4f, 0.9f, 1.0f, alpha };
      for (int k = 0; k < RippleLinks; ++k)
      {
        links[k].colors(color, color);
      }
    }
  }
}

// --- app ---------------------------------------------------------------------------------------

SDL_AppResult App::Init()
{
  SDL_Init(SDL_INIT_VIDEO);
  window = SDL_CreateWindow("Curve Showcase", int(viewport_width), int(viewport_height), SDL_WINDOW_RESIZABLE);
  if (window == NULL)
  {
    return SDL_APP_FAILURE;
  }
  HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

  device = std::make_unique<Axodox::Graphics::GraphicsDevice>(Axodox::Graphics::GraphicsDeviceFlags::UseDebugSdkLayers);

  swapchain = std::make_unique<Axodox::Graphics::HwndSwapChain>(*device, hwnd);
  depth = std::make_unique<DepthStencil2D>(*device, Texture2DDefinition{
    uint32_t(viewport_width), uint32_t(viewport_height),
    DXGI_FORMAT_D32_FLOAT,
    Texture2DFlags::DepthStencil
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  ImGui_ImplSDL3_InitForD3D(window);
  ImGui_ImplDX11_Init(device->get(), device->ImmediateContext()->get());

  camera.SetProj(viewport_width / viewport_height, 0.1f);
  camera.SetView(XMVECTOR{ 0.0f, 7.0f, -27.0f, 0.0f }, XMVECTOR{ 0.0f, 0.0f, 0.0f, 0.0f }, XMVECTOR{ 0.0f, 1.0f, 0.0f, 0.0f });
  orbital_manipulator.SetCamera(&camera);
  buffer_camera = std::make_unique<Axodox::Graphics::ConstantBuffer>(*device, camera.GetData());

  CreateMeshPass();

  curves = std::make_unique<BezierRenderer>(*device);
  curves->SetViewport(viewport_width, viewport_height);
  BuildScene();
  PoseScene();

  return SDL_APP_CONTINUE;
}

void App::Update(float delta)
{
  orbital_manipulator.Update(delta);
  if (!paused)
  {
    animation_time += delta * time_scale;
    PoseScene();
  }
}

void App::Gui()
{
  if (!show_ui) return;

  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Showcase"))
  {
    ImGui::Text("%.1f FPS (%.2f ms/frame)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Text("curves %u: solid %u / patterned %u / dots %u",
      unsigned(curves->Count()),
      unsigned(curves->Count(CurveKind::Solid)),
      unsigned(curves->Count(CurveKind::Patterned)),
      unsigned(curves->Count(CurveKind::Dotted)));

    ImGui::Checkbox("Pause", &paused);
    ImGui::SameLine();
    ImGui::Checkbox("Meshes", &draw_meshes);
    ImGui::SameLine();
    ImGui::Checkbox("Curves", &draw_curves);
    ImGui::SliderFloat("Speed", &time_scale, 0.0f, 3.0f, "%.2fx");
    if (ImGui::SliderFloat("Time", &animation_time, 0.0f, 120.0f, "%.2f s"))
    {
      PoseScene();
    }
    ImGui::TextDisabled("F1 hides this window");
  }
  ImGui::End();
}

void App::Render()
{
  DrawEnvironment();

  buffer_camera->Bind(Axodox::Graphics::ShaderStage::Vertex, 0);
  buffer_camera->Bind(Axodox::Graphics::ShaderStage::Pixel, 0);

  if (draw_curves) curves->Draw(*device, camera.GetViewProj());
}

SDL_AppResult App::Iterate(float delta)
{
  Update(delta);
  buffer_camera->Upload(camera.GetData());

  ImGui_ImplDX11_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  Gui();
  ImGui::Render();

  auto* back = swapchain->BackBuffer();
  back->Clear({ 0.0f, 0.0f, 0.0f, 1.0f });
  depth->Clear(0.0f);
  device->ImmediateContext()->BindRenderTargets(back, depth.get());
  Render();
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  swapchain->Present();
  return SDL_APP_CONTINUE;
}

SDL_AppResult App::Event(const SDL_Event& Event)
{
  ImGui_ImplSDL3_ProcessEvent(&Event);
  const ImGuiIO& io = ImGui::GetIO();
  if (Event.type == SDL_EVENT_KEY_DOWN && Event.key.key == SDLK_F1 && Event.key.repeat == 0)
  {
    show_ui = !show_ui;
  }
  if (!io.WantCaptureKeyboard)
  {
    if (Event.type == SDL_EVENT_KEY_DOWN)
    {
      if (Event.key.key == SDLK_SPACE && Event.key.repeat == 0) paused = !paused;
      orbital_manipulator.KeyboardDown(Event.key);
    }
    if (Event.type == SDL_EVENT_KEY_UP)
    {
      orbital_manipulator.KeyboardUp(Event.key);
    }
  }
  if (!io.WantCaptureMouse)
  {
    if (Event.type == SDL_EVENT_MOUSE_MOTION)
    {
      orbital_manipulator.MouseMove(Event.motion);
    }
    if (Event.type == SDL_EVENT_MOUSE_WHEEL)
    {
      orbital_manipulator.MouseWheel(Event.wheel);
    }
  }
  if (Event.type == SDL_EVENT_WINDOW_RESIZED)
  {
    viewport_width = float(Event.window.data1);
    viewport_height = float(Event.window.data2);

    camera.SetAspect(viewport_width / viewport_height);
    curves->SetViewport(viewport_width, viewport_height);
    swapchain->Resize();
    depth = std::make_unique<DepthStencil2D>(*device, Texture2DDefinition{
      uint32_t(viewport_width), uint32_t(viewport_height),
      DXGI_FORMAT_D32_FLOAT,
      Texture2DFlags::DepthStencil
    });
  }
  return Event.type == SDL_EVENT_QUIT ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

void App::Quit(SDL_AppResult result)
{
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();

  SDL_DestroyWindow(window);
}
