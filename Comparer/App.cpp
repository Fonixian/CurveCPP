#include "App.h"
#include <d3d11.h>
#include <DirectXMath.h>
#include <imgui.h>
#include "imgui_impl_sdl3.h"
#include "imgui_impl_dx11.h"
#include "Include/Axodox.Storage.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

using namespace Axodox::Storage;
using namespace Axodox::Graphics;
using namespace DirectX;

namespace
{
  constexpr float Tau = 6.28318530718f;

  // --- scene layout, in one copy's LOCAL coordinates -----------------------------------------
  // Everything below lives in x in [-3, 3], y in [-4.4, 4.6]; the whole box is then slid sideways
  // by Scene::x_offset. Keep new curves inside that box or the two copies will start to overlap.
  constexpr float GalleryBottom = 3.2f; // gallery strokes run bottom (front) to top (back)
  constexpr float GalleryTop = 4.6f;
  constexpr float GalleryStride = 1.25f; // world units between neighbouring gallery strokes

  constexpr float PetalCenterY = 0.7f;
  constexpr float PetalRadius = 1.45f;

  constexpr float HermiteRadius = 0.75f; // the Hermite loop sits inside the petal ring

  constexpr float RibbonY = -2.3f;
  constexpr float RibbonHalfSpan = 3.0f;
  constexpr float RibbonAmplitude = 0.55f;
  constexpr float RibbonDepth = 2.0f; // how far the ribbon dives toward / away from the camera

  constexpr float WaveY = -3.9f;

  // Index order must match the CurveCap enum in BezierData.h.
  const char* const CapNames[] = { "Butt", "Square", "Round", "Triangle out", "Triangle in" };
  constexpr int CapCount = int(sizeof(CapNames) / sizeof(CapNames[0]));

  // Gallery stroke i is capped `i` at the front and `i + 2` at the back, so every cap shows up
  // once at each end and no stroke wears the same cap twice.
  CurveCap GalleryFrontCap(int i)
  {
    return static_cast<CurveCap>(i % CapCount);
  }

  CurveCap GalleryBackCap(int i)
  {
    return static_cast<CurveCap>((i + 2) % CapCount);
  }

  XMFLOAT3 Add3(const XMFLOAT3& a, const XMFLOAT3& b)
  {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
  }

  // Hue sweep used for the animated colouring, kept in one place so every group agrees.
  XMFLOAT3 HueColor(float hue)
  {
    return {
      cosf(hue * 3.14159f),
      cosf(hue * 3.14159f + 2.09f),
      cosf(hue * 3.14159f + 4.19f)
    };
  }

  // --- Test screen ---------------------------------------------------------------------------
  // Index order must match App::test_degree_index; the degree is the index + 1.
  const char* const DegreeNames[] = { "Linear (2 points)", "Quadratic (3 points)", "Cubic (4 points)" };
  constexpr int DegreeCount = int(sizeof(DegreeNames) / sizeof(DegreeNames[0]));

  // Fixed seed on purpose. A performance comparison is only worth anything if pressing Add twice in
  // two different builds produces the same curves, so this is deterministic per run rather than
  // seeded from the clock. It still advances between presses, so consecutive batches differ.
  std::mt19937 test_rng{ 0x5EED1234u };

  float RandRange(float low, float high)
  {
    return std::uniform_real_distribution<float>(low, high)(test_rng);
  }

  XMFLOAT3 RandInBox(const XMFLOAT3& half_extents)
  {
    return {
      RandRange(-half_extents.x, half_extents.x),
      RandRange(-half_extents.y, half_extents.y),
      RandRange(-half_extents.z, half_extents.z)
    };
  }
}

void App::BuildScene(CurveRenderer& renderer, Scene& scene, float x_offset)
{
  scene.x_offset = x_offset;

  // Initial poses barely matter - PoseScene() overwrites every control point and colour on the
  // very first frame. They only have to be valid enough for Add()'s asserts.
  BezierData wave = Cubic(
    { -3.0f, WaveY - 0.5f, 0.0f },
    { -1.5f, WaveY + 0.7f, 0.0f },
    { 1.5f, WaveY + 0.7f, 0.0f },
    { 3.0f, WaveY - 0.5f, 0.0f });
  wave.style.C0 = wave_color0;
  wave.style.C1 = wave_color1;
  scene.wave = renderer.Add(wave);

  for (int i = 0; i < PetalCount; ++i)
  {
    BezierData petal = Quadratic(
      { PetalRadius, PetalCenterY, 0.0f },
      { PetalRadius, PetalCenterY, 0.0f },
      { PetalRadius, PetalCenterY, 0.0f });
    petal.style.C0 = petal_color;
    petal.style.C1 = petal_color;
    scene.petals[i] = renderer.Add(petal);
  }

  // The ribbon and the Hermite loop are each one merged stroke; every other group in the scene
  // (wave, petals, gallery, test curves) is Added unmerged, i.e. one stroke per curve.
  {
    BezierStyle style;
    style.C0 = ribbon_color0;
    style.C1 = ribbon_color1;
    scene.ribbon = CatmullRomSpline(renderer, style, RibbonLinks + 1);
  }

  {
    BezierStyle style;
    style.C0 = hermite_color0;
    style.C1 = hermite_color1;
    scene.hermite = HermiteSpline(renderer, style, HermiteLinks + 1);
  }

  // The cap gallery is the one group whose caps are NOT driven by the style panel: each stroke
  // keeps the pair it was built with, so all five caps stay on screen at once while you change
  // everything else around them.
  for (int i = 0; i < GalleryCount; ++i)
  {
    BezierData stroke = Linear(
      { 0.0f, GalleryBottom, 0.0f },
      { 0.0f, GalleryTop, 0.0f });
    stroke.style.cap_front = GalleryFrontCap(i);
    stroke.style.cap_back = GalleryBackCap(i);
    stroke.style.resolution = GalleryResolution;
    stroke.style.C0 = { 1.0f, 1.0f, 1.0f };
    stroke.style.C1 = gallery_color;
    scene.gallery[i] = renderer.Add(stroke);
  }
}

void App::AddTestCurves(CurveRenderer& renderer, Scene& scene)
{
  const auto cap_front = static_cast<CurveCap>(style_cap_front);
  const auto cap_back = static_cast<CurveCap>(link_caps ? style_cap_front : style_cap_back);
  const int degree = test_degree_index + 1;
  const auto resolution = static_cast<unsigned>(test_resolution);

  scene.test_curves.reserve(scene.test_curves.size() + size_t(test_count));

  for (int i = 0; i < test_count; ++i)
  {
    TestCurve test;
    test.degree = degree;
    test.resolution = resolution;

    // An origin somewhere in the box, then each control point scattered around it. Picking every
    // control point independently across the whole box instead would give curves that all span the
    // full extent and sit on top of each other; scattering keeps them local and lets a batch read
    // as many separate strokes, which is what makes an overdraw difference visible.
    const XMFLOAT3 origin = RandInBox(test_range);
    const XMFLOAT3 spread = { test_spread, test_spread, test_spread };
    for (int k = 0; k <= degree; ++k)
    {
      test.P[k] = Add3(origin, RandInBox(spread));
    }

    // Control points go in already offset into this renderer's column; PoseScene() keeps them
    // there afterwards from the LOCAL copy held in TestCurve::P.
    const float dx = scene.x_offset;
    XMFLOAT3 placed[4];
    for (int k = 0; k <= degree; ++k)
    {
      placed[k] = { test.P[k].x + dx, test.P[k].y, test.P[k].z };
    }

    BezierData data;
    switch (degree)
    {
    case 1: data = Linear(placed[0], placed[1]); break;
    case 2: data = Quadratic(placed[0], placed[1], placed[2]); break;
    default: data = Cubic(placed[0], placed[1], placed[2], placed[3]); break;
    }

    // The properties are the ones the window is showing - only the geometry is random. Everything
    // here is what ApplyStyle() would push anyway, set at Add time so a curve looks right on the
    // frame it appears rather than on the next one - and, in the BezierRenderer, lands straight in
    // the backend the current pattern picks instead of being moved there on the next Draw().
    data.style.width = style_width;
    data.style.cap_front = cap_front;
    data.style.cap_back = cap_back;
    data.style.join = static_cast<CurveJoin>(style_join);
    data.style.dash_length = style_dash_length;
    data.style.spacing = style_patterned ? style_spacing : 0.0f;
    data.style.min_height = style_min_height;
    data.style.max_height = style_max_height;
    data.style.resolution = resolution;

    // Hue by index so a batch is legible as separate strokes rather than one solid mass. Test
    // curves are not repainted afterwards, so this is the only place their colour is decided.
    const XMFLOAT3 color = HueColor(float(scene.test_curves.size()) * 0.137f);
    data.style.C0 = color;
    data.style.C1 = HueColor(float(scene.test_curves.size()) * 0.137f + 0.5f);

    test.handle = renderer.Add(data);
    // Moved, not copied: the handle owns the curve and is move-only.
    scene.test_curves.push_back(std::move(test));
  }

  // The new handles still have to be picked up by both per-frame passes: PoseScene() so they track
  // their column, ApplyStyle() so a later style change reaches them.
  scene_dirty = true;
  style_dirty = true;
}

void App::RemoveTestCurves(Scene& scene, size_t count)
{
  const size_t keep = scene.test_curves.size() - std::min(count, scene.test_curves.size());
  // Each erased TestCurve destroys its handle, which removes the curve from the renderer. The
  // renderer only marks it and compacts on its next UpdateBuffers, so erasing a whole batch costs
  // one pass there, not one per curve. The survivors need no re-pose or re-style: their data is
  // untouched, only their index shifts, and the handles are told about that by the renderer.
  scene.test_curves.erase(scene.test_curves.begin() + std::ptrdiff_t(keep), scene.test_curves.end());
}

void App::PoseScene(Scene& scene)
{
  const float dx = scene.x_offset;
  const float t = animation_time;

  // --- wave: one cubic swinging its two inner control points sideways -------------------------
  {
    const float swing = sinf(t * 2.0f);

    XMFLOAT3 c0 = wave_color0;
    XMFLOAT3 c1 = wave_color1;
    if (animate_colors)
    {
      c0 = { 1.0f - t / 10.0f, 0.5f, 1.0f };
      c1 = { 0.2f, 1.0f, 0.5f - t / 10.0f };
    }

    scene.wave.control_points(
      { dx - 3.0f, WaveY - 0.5f, 0.0f },
      { dx - 1.5f + swing, WaveY + 0.7f, 0.0f },
      { dx + 1.5f + swing, WaveY + 0.7f, 0.0f },
      { dx + 3.0f, WaveY - 0.5f, 0.0f });
    scene.wave.colors(c0, c1);
  }

  // --- petals: quadratic arcs spaced around a rotating ring, with gaps between them ------------
  for (int i = 0; i < PetalCount; ++i)
  {
    const float angle = t + (Tau * i / PetalCount);
    const float nextAngle = angle + (Tau / (2.0f * PetalCount));
    const float midAngle = (angle + nextAngle) * 0.5f;
    const float controlRadius = PetalRadius * 1.5f;

    const XMFLOAT3 P0 = { dx + PetalRadius * cosf(angle), PetalCenterY + PetalRadius * sinf(angle), 0.0f };
    const XMFLOAT3 P2 = { dx + PetalRadius * cosf(nextAngle), PetalCenterY + PetalRadius * sinf(nextAngle), 0.0f };
    const XMFLOAT3 P1 = { dx + controlRadius * cosf(midAngle), PetalCenterY + controlRadius * sinf(midAngle), 0.0f };

    const XMFLOAT3 color = animate_colors ? HueColor(angle / Tau) : petal_color;

    scene.petals[i].control_points(P0, P1, P2);
    scene.petals[i].colors(color, color);
  }

  // --- ribbon: a Catmull-Rom spline along a 3D path --------------------------------------------
  // Six cubic links through seven points, merged into ONE stroke: one arc length, one pattern grid,
  // caps only at the two far ends.
  {
    const float step = 1.0f / RibbonLinks;

    // Defined outside [0, 1] too, which is what lets the end points come from the path itself
    // rather than being mirrored.
    auto ribbon_point = [&](float s)
    {
      return XMFLOAT3{
        dx + (s * 2.0f - 1.0f) * RibbonHalfSpan,
        RibbonY + RibbonAmplitude * sinf(s * Tau * 1.5f + t * 1.3f),
        RibbonDepth * cosf(s * Tau + t * 0.7f)
      };
    };

    auto& ribbon = scene.ribbon;
    for (size_t k = 0; k < ribbon.size(); ++k)
    {
      ribbon[k] = ribbon_point(float(k) * step);
    }
    ribbon.FrontEnd() = ribbon_point(-step);
    ribbon.BackEnd() = ribbon_point(float(ribbon.size()) * step);
    ribbon.Update();

    if (animate_colors)
    {
      XMFLOAT3 colors[RibbonLinks + 1];
      for (int k = 0; k <= RibbonLinks; ++k)
      {
        colors[k] = HueColor(float(k) * step + t * 0.15f);
      }
      ribbon.PointColors(colors);
    }
    else
    {
      ribbon.Colors(ribbon_color0, ribbon_color1);
    }
  }

  // --- hermite: a closed loop whose tangents twist and stretch --------------------------------
  // Knots sit on a breathing circle. Tangents along the circle, r * dtheta long, give a round loop;
  // twisting them makes a pinwheel, shrinking them flattens it to a polygon.
  {
    auto& hermite = scene.hermite;
    const size_t links = hermite.size() - 1;
    const float step = Tau / float(links);
    const float twist = 1.2f * sinf(t * 0.8f);
    const float stretch = 1.0f + 0.8f * sinf(t * 1.3f);
    const float ct = cosf(twist);
    const float st = sinf(twist);

    for (size_t k = 0; k < links; ++k)
    {
      const float angle = float(k) * step + t * 0.4f;
      const float r = HermiteRadius * (1.0f + 0.2f * sinf(3.0f * angle + t * 1.1f));
      const float tx = -sinf(angle) * r * step * stretch;
      const float ty = cosf(angle) * r * step * stretch;

      hermite[k].position = { dx + r * cosf(angle), PetalCenterY + r * sinf(angle), 0.0f };
      hermite[k].tangent = { tx * ct - ty * st, tx * st + ty * ct, 0.0f };
    }
    hermite[links] = hermite[0]; // closes the loop exactly
    hermite.Update();

    if (animate_colors)
    {
      XMFLOAT3 colors[HermiteLinks + 1];
      for (int k = 0; k < HermiteLinks; ++k)
      {
        colors[k] = HueColor(2.0f * float(k) / HermiteLinks - t * 0.2f); // one full hue period around
      }
      colors[HermiteLinks] = colors[0];
      hermite.PointColors(colors);
    }
    else
    {
      hermite.Colors(hermite_color0, hermite_color1);
    }
  }

  // --- cap gallery: five straight strokes, one per cap pair ------------------------------------
  // Static apart from following x_offset. The front (P0) end is white so it is obvious which cap
  // is which way round.
  for (int i = 0; i < GalleryCount; ++i)
  {
    const float x = dx + (i - (GalleryCount - 1) * 0.5f) * GalleryStride;

    scene.gallery[i].control_points(
      { x, GalleryBottom, 0.0f },
      { x, GalleryTop, 0.0f });

    const XMFLOAT3 back = animate_colors ? HueColor(float(i) / GalleryCount + t * 0.1f) : gallery_color;
    scene.gallery[i].colors({ 1.0f, 1.0f, 1.0f }, back);
  }

  // --- test curves: static geometry, slid into this copy's column ------------------------------
  // They do not animate; the only thing this loop does is re-apply x_offset, so that a test batch
  // added while both renderers were visible stays in its own column after one is unticked and the
  // layout re-centres. Colours were fixed at Add time and are left alone.
  for (auto& test : scene.test_curves)
  {
    XMFLOAT3 placed[4];
    for (int k = 0; k <= test.degree; ++k)
    {
      placed[k] = { test.P[k].x + dx, test.P[k].y, test.P[k].z };
    }

    switch (test.degree)
    {
    case 1: test.handle.control_points(placed[0], placed[1]); break;
    case 2: test.handle.control_points(placed[0], placed[1], placed[2]); break;
    default: test.handle.control_points(placed[0], placed[1], placed[2], placed[3]); break;
    }
  }
}

void App::ApplyStyle(Scene& scene)
{
  const auto cap_front = static_cast<CurveCap>(style_cap_front);
  const auto cap_back = static_cast<CurveCap>(link_caps ? style_cap_front : style_cap_back);

  // `target`, not `curve` - a local called `curve` would shadow nothing here today, but it does
  // in any translation unit that also names the enums through a namespace, so keep the habit.
  // By reference: handles own their curve, so a by-value copy is not just wasteful - it does not exist.
  // Generic so it takes a CatmullRomSpline as well, whose setters mirror BezierCurve's.
  auto apply = [&](auto& target, bool keep_own_caps, unsigned resolution)
  {
    target.Width(style_width);
    if (!keep_own_caps)
    {
      target.Cap(cap_front, cap_back);
    }
    target.Join(static_cast<CurveJoin>(style_join));
    // Spacing and dash length decide the backend in the BezierRenderer (KindOf in BezierData.h): spacing
    // 0 is solid, dash length 0 dots, both set dashes. The setters only record the values; a curve
    // whose kind changed moves on the renderer's next Draw(). The line renderer ignores both.
    target.DashLength(style_dash_length);
    target.Spacing(style_patterned ? style_spacing : 0.0f);
    target.HeightRange(style_min_height, style_max_height);

    // Resolution changes force a layout rebuild, so only push it when it actually moved rather
    // than every frame.
    if (target.Resolution() != resolution)
    {
      target.Resolution(resolution);
    }
  };

  const auto resolution = static_cast<unsigned>(style_resolution);

  apply(scene.wave, false, resolution);
  for (auto& petal : scene.petals)
  {
    apply(petal, false, resolution);
  }
  apply(scene.ribbon, false, resolution);
  apply(scene.hermite, false, resolution);

  // The gallery keeps the cap pair it was built with - that is the whole point of it - and stays
  // at two points, because a straight line gains nothing from subdivision.
  for (auto& stroke : scene.gallery)
  {
    apply(stroke, true, GalleryResolution);
  }

  // Test curves follow every style control EXCEPT resolution: each keeps the one it was added with,
  // so a scene can mix a batch at 8 points with a batch at 400 and the Timings table shows what that
  // costs. The Stroke section's Resolution slider only ever moves the example groups.
  for (auto& test : scene.test_curves)
  {
    apply(test.handle, false, test.resolution);
  }
}

SDL_AppResult App::Init()
{
  SDL_Init(SDL_INIT_VIDEO);
  window = SDL_CreateWindow("Curve Renderer Comparison", int(viewport_width), int(viewport_height), SDL_WINDOW_RESIZABLE);
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
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

  ImGui::StyleColorsDark();

  ImGui_ImplSDL3_InitForD3D(window);
  ImGui_ImplDX11_Init(device->get(), device->ImmediateContext()->get());

  camera.SetProj(viewport_width / viewport_height, 0.1f, 1000.0f);
  // Far enough back that the copies of the scene fit side by side. The orbital manipulator
  // picks its orbit distance up from this, so change it here rather than in OrbitalCamera.
  camera.SetView(XMVECTOR{ 0.0f, 0.0f, 0.0f, 0.0f }, XMVECTOR{ 0.0f, 0.0f, -19.0f, 0.0f }, XMVECTOR{ 0.0f, 1.0f, 0.0f, 0.0f });
  orbital_manipulator.SetCamera(&camera);
  buffer_camera = std::make_unique<Axodox::Graphics::ConstantBuffer>(*device, camera.GetData());

  curve_renderer = std::make_unique<BezierRenderer>(*device);
  line_renderer = std::make_unique<BezierLineRenderer>(*device);

  curve_renderer->SetViewport(viewport_width, viewport_height);
  line_renderer->SetViewport(viewport_width, viewport_height);

  // Initial offsets here only need to be valid enough for BuildScene()'s local coordinates to stay
  // inside their box; Update() recomputes the real layout (which depends on which renderers are
  // visible) before the first frame is drawn.
  BuildScene(*curve_renderer, curve_scene, -0.5f * compare_offset);
  BuildScene(*line_renderer, line_scene, 0.5f * compare_offset);

  PoseScene(curve_scene);
  PoseScene(line_scene);
  ApplyStyle(curve_scene);
  ApplyStyle(line_scene);

  return SDL_APP_CONTINUE;
}

void App::Update(float delta)
{
  orbital_manipulator.Update(delta);

  bool repose = scene_dirty;
  scene_dirty = false;

  if (!paused)
  {
    animation_time += delta;
    repose = true;
  }

  // Evenly space whichever columns are visible, in the fixed left-to-right order Bezier / Lines,
  // centred on the origin. A hidden renderer's offset doesn't matter - it is never drawn - but its
  // scene is still kept current below, same as before.
  {
    const bool visible[ColumnCount] = { draw_curves, draw_lines };
    const int visible_count = int(draw_curves) + int(draw_lines);

    float offsets[ColumnCount] = { 0.0f, 0.0f };
    int slot = 0;
    for (int i = 0; i < ColumnCount; ++i)
    {
      if (!visible[i]) continue;
      offsets[i] = visible_count > 1 ? (float(slot) - (visible_count - 1) * 0.5f) * compare_offset : 0.0f;
      ++slot;
    }

    // A moved column has to be re-posed even while paused, otherwise the curves stay behind at the
    // old offset - so ticking a renderer or dragging the comparison gap counts as a scene change.
    Scene* scenes[ColumnCount] = { &curve_scene, &line_scene };
    for (int i = 0; i < ColumnCount; ++i)
    {
      if (scenes[i]->x_offset == offsets[i]) continue;
      scenes[i]->x_offset = offsets[i];
      repose = true;
    }
  }

  // Both copies are kept up to date even while hidden: the setters only raise dirty flags, and the
  // actual upload (and, in the BezierRenderer, any move between backends) happens inside Draw(),
  // which a hidden renderer never reaches.
  //
  // Both passes call touch() on every curve they visit, and need_upload is coarse - one touched
  // curve re-uploads the entire buffer - so neither runs unless something actually changed. That is
  // what makes "Pause animation" free, which is what makes the Test screen's numbers mean anything
  // once the scene is a few thousand curves deep.
  if (repose)
  {
    PoseScene(curve_scene);
    PoseScene(line_scene);
  }

  if (style_dirty)
  {
    ApplyStyle(curve_scene);
    ApplyStyle(line_scene);
    style_dirty = false;
  }
}

void App::Gui()
{
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(380, 700), ImGuiCond_FirstUseEver);

  if (ImGui::Begin("Curve Style"))
  {
    // --- shared header: true on both screens ------------------------------------------------
    ImGui::Text("%.1f FPS (%.2f ms/frame)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::Checkbox("Pause animation", &paused);
    ImGui::SameLine();
    ImGui::Checkbox("Timings window", &show_timings);
    ImGui::TextDisabled("Paused now skips the per-frame re-upload entirely.");

    ImGui::SeparatorText("Renderers");
    ImGui::Checkbox("Bezier", &draw_curves);
    ImGui::SameLine();
    ImGui::Checkbox("Lines", &draw_lines);

    // The Test screen adds to one renderer at a time, so the two counts are allowed to differ. The
    // Bezier renderer's split is where its curves were routed by the last Draw().
    ImGui::TextDisabled("curves: Bezier %u (solid %u / patterned %u / dots %u), Lines %u",
      unsigned(curve_renderer->Count()),
      unsigned(curve_renderer->Count(CurveKind::Solid)),
      unsigned(curve_renderer->Count(CurveKind::Patterned)),
      unsigned(curve_renderer->Count(CurveKind::Dotted)),
      unsigned(line_renderer->Count()));

    const int visible_renderer_count = int(draw_curves) + int(draw_lines);
    ImGui::BeginDisabled(visible_renderer_count <= 1);
    ImGui::SliderFloat("Comparison gap", &compare_offset, 0.0f, 14.0f, "%.1f world units");
    ImGui::EndDisabled();

    // --- the two screens ---------------------------------------------------------------------
    if (ImGui::BeginTabBar("screens"))
    {
      if (ImGui::BeginTabItem("Example"))
      {
        ExampleGui();
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Test"))
      {
        TestGui();
        ImGui::EndTabItem();
      }

      ImGui::EndTabBar();
    }
  }

  ImGui::End();

  if (show_timings)
  {
    ProfilerGui();
  }
}

void App::ExampleGui()
{
  ImGui::TextWrapped(
    "Left: the Bezier renderer. It sends every curve to one of three backends by its pattern - "
    "spacing 0 to Solid, dash length 0 to Dots, both set to Patterned - and moves it there again "
    "whenever the pattern changes, so the Pattern section below moves the whole scene from one "
    "backend to another; the Timings window shows each backend in a column of its own. Right: "
    "BezierLineRenderer, the hardware line primitive - colour only, 1 px, no caps, joins or "
    "pattern - as the cost floor.");

  StyleGui();

  // Colours belong to this screen: they drive the hand-built groups only. Test curves get a
  // fixed hue at Add time and are never repainted.
  ImGui::SeparatorText("Colour");
  bool changed = ImGui::Checkbox("Animate colours", &animate_colors);
  ImGui::BeginDisabled(animate_colors);
  changed |= ImGui::ColorEdit3("Wave A", &wave_color0.x);
  changed |= ImGui::ColorEdit3("Wave B", &wave_color1.x);
  changed |= ImGui::ColorEdit3("Petals", &petal_color.x);
  changed |= ImGui::ColorEdit3("Ribbon A", &ribbon_color0.x);
  changed |= ImGui::ColorEdit3("Ribbon B", &ribbon_color1.x);
  changed |= ImGui::ColorEdit3("Hermite A", &hermite_color0.x);
  changed |= ImGui::ColorEdit3("Hermite B", &hermite_color1.x);
  changed |= ImGui::ColorEdit3("Gallery", &gallery_color.x);
  ImGui::EndDisabled();

  // Colours are written by PoseScene(), which no longer runs every frame - so a picker moved while
  // paused has to say so or nothing would happen until the animation is resumed.
  if (changed)
  {
    scene_dirty = true;
  }
}

void App::TestGui()
{
  ImGui::TextWrapped(
    "Spawns random curves into ONE renderer at a time - the two scenes are allowed to diverge, "
    "which is the only way to give one renderer a heavier load than the other and watch the "
    "Timings table split. Only the geometry is random: every other property comes from the "
    "controls below, the same ones the example scene is drawn with. In the Bezier renderer the "
    "pattern decides which backend a batch lands in, and changing it later moves the batch.");

  ImGui::SeparatorText("Batch");
  ImGui::Combo("Degree", &test_degree_index, DegreeNames, DegreeCount);
  ImGui::SliderInt("Count", &test_count, 1, 2000);
  // Deliberately a separate slider from the Stroke section's Resolution, which is also on this tab:
  // this one is baked into each curve at Add time and never moves again, so batches at different
  // resolutions can coexist in one scene. Named differently so the two are not confusable on screen.
  ImGui::SliderInt("New curve resolution", &test_resolution, 2, 400);
  ImGui::TextDisabled("One press adds `count` curves, all at that resolution.");

  ImGui::SeparatorText("Random placement");
  ImGui::SliderFloat3("Range", &test_range.x, 0.0f, 8.0f, "%.1f");
  ImGui::SliderFloat("Spread", &test_spread, 0.05f, 4.0f, "%.2f");
  ImGui::TextWrapped(
    "Range is the half-extent of the box a curve's origin falls in, measured in its own column's "
    "local coordinates; spread is how far that curve's control points scatter around the origin. "
    "One copy of the example scene occupies about 3 x 4.5, so a range near that keeps a batch "
    "inside its own column instead of sprawling across the neighbouring one.");

  ImGui::SeparatorText("Add");
  if (ImGui::Button("Add to Bezier"))
  {
    AddTestCurves(*curve_renderer, curve_scene);
  }
  ImGui::SameLine();
  if (ImGui::Button("Add to Lines"))
  {
    AddTestCurves(*line_renderer, line_scene);
  }

  // Rewinding the generator between the two calls makes them the SAME curves rather than two
  // independent batches, which is the only way the two columns stay a like-for-like comparison.
  if (ImGui::Button("Add to both (identical curves)"))
  {
    const std::mt19937 batch_start = test_rng;
    AddTestCurves(*curve_renderer, curve_scene);
    test_rng = batch_start;
    AddTestCurves(*line_renderer, line_scene);
  }

  // Removal goes through the handles: erasing a TestCurve destroys its BezierCurve, which takes the
  // curve out of that renderer. The buffers follow on the next Draw - they shrink once what is left
  // fits in a quarter of them, which the Timings window's pattern bound / allocated columns show.
  ImGui::SeparatorText("Remove");
  ImGui::TextDisabled("\"Last batch\" = the last `count` test curves of that renderer.");
  const size_t batch = size_t(test_count);
  ImGui::BeginDisabled(curve_scene.test_curves.empty());
  if (ImGui::Button("Last batch: Bezier"))
  {
    RemoveTestCurves(curve_scene, batch);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(line_scene.test_curves.empty());
  if (ImGui::Button("Last batch: Lines"))
  {
    RemoveTestCurves(line_scene, batch);
  }
  ImGui::EndDisabled();

  if (ImGui::Button("Last batch from both"))
  {
    RemoveTestCurves(curve_scene, batch);
    RemoveTestCurves(line_scene, batch);
  }
  ImGui::SameLine();
  if (ImGui::Button("Remove all test curves"))
  {
    RemoveTestCurves(curve_scene, curve_scene.test_curves.size());
    RemoveTestCurves(line_scene, line_scene.test_curves.size());
  }

  ImGui::SeparatorText("In the scene");

  // Sample points, not curves, is what the point pass and the buffers are actually sized by, so it
  // is the number worth watching next to the Timings window.
  auto sample_points = [](const Scene& scene)
  {
    size_t total = 0;
    for (const auto& test : scene.test_curves) total += test.resolution;
    return total;
  };

  constexpr ImGuiTableFlags table_flags =
    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

  if (ImGui::BeginTable("test_counts", ColumnCount + 1, table_flags))
  {
    const Scene* scenes[ColumnCount] = { &curve_scene, &line_scene };
    const char* names[ColumnCount] = { "Bezier", "Lines" };

    ImGui::TableSetupColumn("");
    for (int i = 0; i < ColumnCount; i++) ImGui::TableSetupColumn(names[i]);
    ImGui::TableHeadersRow();

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("test curves");
    for (int i = 0; i < ColumnCount; i++)
    {
      ImGui::TableNextColumn();
      ImGui::Text("%u", unsigned(scenes[i]->test_curves.size()));
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("their points");
    for (int i = 0; i < ColumnCount; i++)
    {
      ImGui::TableNextColumn();
      ImGui::Text("%u", unsigned(sample_points(*scenes[i])));
    }

    ImGui::EndTable();
  }

  StyleGui();
}

void App::StyleGui()
{
  bool changed = false;

  ImGui::SeparatorText("Stroke");
  changed |= ImGui::SliderFloat("Width (px)", &style_width, 1.0f, 40.0f, "%.1f");
  // Only the example groups follow this; every test curve keeps the resolution it was added with,
  // which is why the Test tab has a slider of its own for that.
  changed |= ImGui::SliderInt("Resolution (example scene)", &style_resolution, 2, 400);

  ImGui::SeparatorText("Caps");
  changed |= ImGui::Checkbox("Same cap at both ends", &link_caps);
  changed |= ImGui::Combo("Front cap", &style_cap_front, CapNames, CapCount);

  // While linked the back picker mirrors the front one instead of showing a stale value.
  int shown_back_cap = link_caps ? style_cap_front : style_cap_back;
  ImGui::BeginDisabled(link_caps);
  if (ImGui::Combo("Back cap", &shown_back_cap, CapNames, CapCount))
  {
    style_cap_back = shown_back_cap;
    changed = true;
  }
  ImGui::EndDisabled();
  ImGui::TextWrapped(
    "The row of straight strokes at the top ignores these and keeps one fixed pair each, so "
    "every cap stays visible. Their white end is the front.");

  ImGui::SeparatorText("Join");
  changed |= ImGui::RadioButton("Round##join", &style_join, 0);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("Square##join", &style_join, 1);

  ImGui::SeparatorText("Pattern");
  changed |= ImGui::Checkbox("Patterned", &style_patterned);
  ImGui::SameLine();
  ImGui::TextDisabled("(off = spacing 0 = solid)");

  // Both only mean something while there is a spacing: with "Patterned" off every curve is solid.
  ImGui::BeginDisabled(!style_patterned);
  changed |= ImGui::SliderFloat("Dash length (px)", &style_dash_length, 0.0f, 200.0f, "%.1f");
  changed |= ImGui::SliderFloat("Spacing", &style_spacing, 0.05f, 2.0f, "%.2f world units");
  ImGui::EndDisabled();

  // A dot is a zero-length dash: its two caps meet in the middle, so Round leaves a disc of radius
  // `width`. Dash length 0 is also what sends a curve to the Dots backend; the button sets that, the
  // round caps, and a spacing (dots need one).
  if (ImGui::Button("Make dots"))
  {
    style_patterned = true;
    style_dash_length = 0.0f;
    style_cap_front = int(CurveCap::Round);
    style_cap_back = int(CurveCap::Round);
    link_caps = true;
    changed = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("dash length 0 + round caps");

  // The same KindOf() the BezierRenderer applies, on the values ApplyStyle() pushes. The cap gallery
  // follows the pattern too - only its caps are its own.
  const CurveKind kind = KindOf(style_patterned ? style_spacing : 0.0f, style_dash_length);
  ImGui::Text("Bezier renderer backend: %s", CurveKindName(kind));
  ImGui::TextWrapped(
    "Spacing 0 -> Solid, dash length 0 -> Dots, both set -> Patterned. Dash length is the pixel "
    "length of one dash, cap to cap, and both of its ends wear the cap chosen above; a dot is one "
    "cap shape per spacing interval, so Butt draws nothing there. The cap gallery keeps its own "
    "caps, which is why its strokes differ from everything else.");

  ImGui::SeparatorText("Colour by height");
  ImGui::TextWrapped("Min >= max blends by curve t instead of world Y.");
  changed |= ImGui::SliderFloat("Min height", &style_min_height, -5.0f, 5.0f, "%.2f");
  changed |= ImGui::SliderFloat("Max height", &style_max_height, -5.0f, 5.0f, "%.2f");

  // ApplyStyle() re-uploads the whole curve buffer, so it runs only when one of the above moved.
  if (changed)
  {
    style_dirty = true;
  }
}

void App::ProfilerGui()
{
  // One row per profiled stage, in pipeline order. `cpu` picks which channel of the metric the row
  // shows - "total" appears twice because the whole Draw() call is measured on both.
  struct TimingRow
  {
    const char* label;
    const char* metric;
    bool cpu;
  };

  static constexpr TimingRow rows[] = {
    { "update", "update", true },
    { "  points", "upload points", true },
    { "  colors", "upload colors", true },
    { "calc", "calc", false },
    { "scan", "scan", false },
    { "pattern", "pattern", false },
    { "draw", "draw", false },
    { "total cpu", "total", true },
    { "total gpu", "total", false },
  };

  // The Bezier renderer's three backends, then the line renderer. A backend with no curves is not
  // drawn at all (BezierRenderer skips it), so its numbers freeze the same way a hidden renderer's do.
  const BezierRendererBase* renderers[] = {
    &curve_renderer->Backend(CurveKind::Solid),
    &curve_renderer->Backend(CurveKind::Patterned),
    &curve_renderer->Backend(CurveKind::Dotted),
    line_renderer.get()
  };
  const char* renderer_names[] = { "Solid", "Patterned", "Dots", "Lines" };
  const bool renderer_visible[] = {
    draw_curves && renderers[0]->Count() > 0,
    draw_curves && renderers[1]->Count() > 0,
    draw_curves && renderers[2]->Count() > 0,
    draw_lines
  };
  constexpr int renderer_count = 4;

  ImGui::SetNextWindowPos(ImVec2(380, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(400, 250), ImGuiCond_FirstUseEver);

  if (ImGui::Begin("Timings"))
  {
    ImGui::Text("%.1f FPS (%.2f ms/frame)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    ImGui::TextDisabled("milliseconds, exponential moving average");

    constexpr ImGuiTableFlags table_flags =
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("timings", renderer_count + 1, table_flags))
    {
      ImGui::TableSetupColumn("stage");
      for (int i = 0; i < renderer_count; i++) ImGui::TableSetupColumn(renderer_names[i]);
      ImGui::TableHeadersRow();

      for (const auto& row : rows)
      {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.label);

        for (int i = 0; i < renderer_count; i++)
        {
          ImGui::TableNextColumn();

          // A metric only exists once something has measured it, so a stage a renderer does
          // not have (the solid one has no scan and no pattern pass) stays a dash.
          const auto* metric = renderers[i]->profiler.find(row.metric);
          if (!metric)
          {
            ImGui::TextDisabled("-");
            continue;
          }

          const double value = row.cpu ? metric->cpu : metric->gpu;

          // A hidden renderer never reaches Draw(), so its numbers are frozen at whatever they
          // were when it was last drawn - greyed out to say so.
          if (renderer_visible[i]) ImGui::Text("%.3f", value);
          else ImGui::TextDisabled("%.3f", value);
        }
      }

      ImGui::EndTable();
    }

    ImGui::TextWrapped(
      "Solid, Patterned and Dots are the Bezier renderer's backends: each draws only the curves "
      "routed to it, and one with none is skipped (greyed). GPU rows are timestamp queries read "
      "back a few frames late, so they lag the picture slightly. \"pattern\" covers ini + calc "
      "together, and no longer carries a readback stall on the frames that recount, so it "
      "should stay flat across a scene change.");

    ImGui::Separator();
    // Min over every UpdatePatternBound() call (any renderer), per curve.
    const double bound_min_ns = PatternBoundMinTimeNs();
    if (std::isfinite(bound_min_ns)) ImGui::Text("pattern bound min: %.2f ns / curve", bound_min_ns);
    else ImGui::TextDisabled("pattern bound min: not measured yet");
    ImGui::SameLine();
    if (ImGui::Button("Reset min")) ResetPatternBoundMinTime();

    ImGui::Separator();
    ImGui::TextUnformatted("pattern centres");
    ImGui::TextDisabled("the CPU bound and what was allocated from it");

    // The exact GPU count used to sit between these two, mirrored back a few frames late through
    // a
    // that curve_pattern_resolve / dot_resolve wrote. Those passes are gone - the
    // scan's appended total stays on the GPU, where the shaders that need it read it directly -
    // so there is no count to show any more, and no dispatch is worth adding to produce one.
    struct CountRow
    {
      const char* label;
      uint32_t (*value)(const BezierRendererBase*);
    };

    static const CountRow count_rows[] = {
      { "bound", [](const BezierRendererBase* r) { return r->PatternBound(); } },
      { "allocated", [](const BezierRendererBase* r) { return r->PatternCapacity(); } },
    };

    if (ImGui::BeginTable("counts", renderer_count + 1, table_flags))
    {
      ImGui::TableSetupColumn("");
      for (int i = 0; i < renderer_count; i++) ImGui::TableSetupColumn(renderer_names[i]);
      ImGui::TableHeadersRow();

      for (const auto& row : count_rows)
      {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.label);

        for (int i = 0; i < renderer_count; i++)
        {
          ImGui::TableNextColumn();

          // The solid renderer has no pattern buffer at all, so it shows as a dash.
          if (renderers[i]->PatternCapacity() == 0)
          {
            ImGui::TextDisabled("-");
            continue;
          }

          const uint32_t value = row.value(renderers[i]);
          if (renderer_visible[i]) ImGui::Text("%u", value);
          else ImGui::TextDisabled("%u", value);
        }
      }

      ImGui::EndTable();
    }

    ImGui::TextWrapped(
      "\"bound\" is the control-polygon upper bound the CPU computes from the curve data, and "
      "is what the pattern buffer is sized from - no GPU result is involved, so nothing in a "
      "frame waits on one. \"count\" is what the ini pass actually counted, mirrored back "
      "without blocking and so a few frames old; it is a check on the bound, never a size.");
  }

  ImGui::End();
}

void App::Render()
{
  buffer_camera->Bind(Axodox::Graphics::ShaderStage::Vertex, 0);
  buffer_camera->Bind(Axodox::Graphics::ShaderStage::Pixel, 0);

  const auto view_proj = camera.GetViewProj();
  if (draw_curves) curve_renderer->Draw(*device, view_proj);
  if (draw_lines) line_renderer->Draw(*device, view_proj);
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
  back->Clear({ 0.39f, 0.58f, 0.92f, 0.f });
  depth->Clear();
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
  if (!io.WantCaptureKeyboard)
  {
    if (Event.type == SDL_EVENT_KEY_DOWN)
    {
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
    curve_renderer->SetViewport(viewport_width, viewport_height);
    line_renderer->SetViewport(viewport_width, viewport_height);
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
