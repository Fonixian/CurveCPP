#pragma once
#include <SDL3/SDL.h>
#include <Include/Axodox.Graphics.h>
#include <d3d11.h>
#include "OrbitalCamera.h"
#include "BezierRenderer.h"
#include "CatmullRomSpline.h"
#include "HermiteSpline.h"
#include <vector>

// Showcase scene: a sky / terrain / water / mesh pass compiled at runtime from App.cpp, with curves
// of every type and backend drawn over it.
class App
{
private:
  SDL_Window* window;
  std::unique_ptr<Axodox::Graphics::GraphicsDevice> device;
  std::unique_ptr<Axodox::Graphics::HwndSwapChain> swapchain;
  std::unique_ptr<Axodox::Graphics::DepthStencil2D> depth;
  std::unique_ptr<Axodox::Graphics::ConstantBuffer> buffer_camera;

  std::unique_ptr<BezierRenderer> curves;

  Camera camera;
  OrbitalCamera orbital_manipulator;

  float viewport_width = 1600.0f;
  float viewport_height = 900.0f;

  float animation_time = 4.0f;
  float time_scale = 1.0f;
  bool paused = false;
  bool show_ui = true;
  bool draw_meshes = true;
  bool draw_curves = true;

  // --- mesh pass -------------------------------------------------------------------
  struct GpuMesh
  {
    winrt::com_ptr<ID3D11Buffer> vertices;
    winrt::com_ptr<ID3D11Buffer> indices;
    UINT index_count = 0;
  };

  GpuMesh terrain_mesh;
  GpuMesh water_mesh;
  GpuMesh sphere_mesh;
  GpuMesh torus_mesh;
  GpuMesh box_mesh;

  winrt::com_ptr<ID3D11VertexShader> mesh_vs;
  winrt::com_ptr<ID3D11PixelShader> mesh_ps;
  winrt::com_ptr<ID3D11VertexShader> sky_vs;
  winrt::com_ptr<ID3D11PixelShader> sky_ps;
  winrt::com_ptr<ID3D11InputLayout> mesh_layout;
  winrt::com_ptr<ID3D11Buffer> frame_constants;
  winrt::com_ptr<ID3D11Buffer> object_constants;
  winrt::com_ptr<ID3D11RasterizerState> raster_state;
  winrt::com_ptr<ID3D11DepthStencilState> depth_off;
  winrt::com_ptr<ID3D11DepthStencilState> depth_on;
  winrt::com_ptr<ID3D11BlendState> blend_opaque;
  winrt::com_ptr<ID3D11BlendState> blend_alpha;

  // --- curves ----------------------------------------------------------------------
  static constexpr int RingCount = 18;
  static constexpr int RingLinks = 16;
  static constexpr int MoonCount = 3;
  static constexpr int OrbitLinks = 8;
  static constexpr int TrailLinks = 6;
  static constexpr int DnaPoints = 31;
  static constexpr int DnaRungs = 16;
  static constexpr int KnotPoints = 161;
  static constexpr int ObeliskCount = 4;
  static constexpr int WindCount = 40;
  static constexpr int WindPoints = 10;
  static constexpr int CoasterPoints = 61;
  static constexpr int CartCount = 4;
  static constexpr int AuroraCount = 5;
  static constexpr int AuroraPoints = 14;
  static constexpr int BurstCount = 3;
  static constexpr int SparkCount = 30;
  static constexpr int RippleCount = 6;
  static constexpr int RippleLinks = 8;

  std::vector<BezierCurve> ring_links;   // RingCount x RingLinks, each ring one merged chain
  std::vector<HermiteSpline> orbits;     // closed Hermite loops, dotted
  std::vector<BezierCurve> trail_links;  // MoonCount x TrailLinks, fading arcs behind the moons
  CatmullRomSpline dna_a;
  CatmullRomSpline dna_b;
  std::vector<BezierCurve> dna_rungs;    // linear
  CatmullRomSpline knot;                 // dashed torus knot
  std::vector<BezierCurve> routes;       // cubic arrows between the obelisks
  std::vector<CatmullRomSpline> wind;    // dashed streamlines over the terrain
  CatmullRomSpline rail_left;
  CatmullRomSpline rail_right;
  CatmullRomSpline sleepers;             // short wide dashes = railway ties
  std::vector<CatmullRomSpline> aurora;  // wide translucent ribbons
  std::vector<BezierCurve> sparks;       // quadratic; one burst each solid / dotted / dashed
  std::vector<BezierCurve> ripple_links; // RippleCount x RippleLinks on the lake

  void Update(float delta);
  void Gui();
  void Render();

  void CreateMeshPass();
  void DrawEnvironment();
  void DrawMesh(const GpuMesh& mesh, const DirectX::XMMATRIX& world, const DirectX::XMFLOAT4& color,
    const DirectX::XMFLOAT4& color2, const DirectX::XMFLOAT4& params);

  BezierCurve AddCurve(const BezierStyle& style, bool merge);
  void BuildScene();
  void PoseScene();

public:
  SDL_AppResult Init();
  SDL_AppResult Iterate(float delta);
  SDL_AppResult Event(const SDL_Event& Event);
  void Quit(SDL_AppResult result);
};
