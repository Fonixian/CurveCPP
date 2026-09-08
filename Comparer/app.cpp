#include "app.h"
#include <d3d11.h>
#include <DirectXMath.h>
#include <imgui.h>
#include "imgui_impl_sdl3.h"
#include "imgui_impl_dx11.h"
#include "Include/Axodox.Storage.h"
#include <cmath>
#include <iostream>

using namespace Axodox::Storage;
using namespace Axodox::Graphics;
using namespace DirectX;

namespace
{
	constexpr float Tau = 6.28318530718f;

	// --- scene layout, in one copy's LOCAL coordinates -----------------------------------------
	// Everything below lives in x in [-3, 3], y in [-4.4, 4.6]; the whole box is then slid sideways
	// by Scene::x_offset. Keep new curves inside that box or the two copies will start to overlap.
	constexpr float GalleryBottom = 3.2f;   // gallery strokes run bottom (front) to top (back)
	constexpr float GalleryTop = 4.6f;
	constexpr float GalleryStride = 1.25f;  // world units between neighbouring gallery strokes

	constexpr float PetalCenterY = 0.7f;
	constexpr float PetalRadius = 1.45f;

	constexpr float RibbonY = -2.3f;
	constexpr float RibbonHalfSpan = 3.0f;
	constexpr float RibbonAmplitude = 0.55f;
	constexpr float RibbonDepth = 2.0f;     // how far the ribbon dives toward / away from the camera

	constexpr float WaveY = -3.9f;

	// Index order must match the CurveCap enum in bezier_common.h.
	const char* const CapNames[] = { "Butt", "Square", "Round", "Triangle out", "Triangle in" };
	constexpr int CapCount = int(sizeof(CapNames) / sizeof(CapNames[0]));

	// Gallery stroke i is capped `i` at the front and `i + 2` at the back, so every cap shows up
	// once at each end and no stroke wears the same cap twice.
	CurveCap GalleryFrontCap(int i) { return static_cast<CurveCap>(i % CapCount); }
	CurveCap GalleryBackCap(int i) { return static_cast<CurveCap>((i + 2) % CapCount); }

	XMFLOAT3 Add3(const XMFLOAT3& a, const XMFLOAT3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	XMFLOAT3 Sub3(const XMFLOAT3& a, const XMFLOAT3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	XMFLOAT3 Scale3(const XMFLOAT3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	XMFLOAT3 Lerp3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
	{
		return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
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
}

void App::BuildScene(BezierRendererBase& renderer, Scene& scene, float x_offset)
{
	scene.x_offset = x_offset;

	// Initial poses barely matter - PoseScene() overwrites every control point and colour on the
	// very first frame. They only have to be valid enough for Add()'s asserts.
	BezierData wave = Cubic(
		{ -3.0f, WaveY - 0.5f, 0.0f },
		{ -1.5f, WaveY + 0.7f, 0.0f },
		{  1.5f, WaveY + 0.7f, 0.0f },
		{  3.0f, WaveY - 0.5f, 0.0f });
	wave.C0 = wave_color0;
	wave.C1 = wave_color1;
	scene.wave = renderer.Add(wave);

	for (int i = 0; i < PetalCount; ++i)
	{
		BezierData petal = Quadratic(
			{ PetalRadius, PetalCenterY, 0.0f },
			{ PetalRadius, PetalCenterY, 0.0f },
			{ PetalRadius, PetalCenterY, 0.0f });
		petal.C0 = petal_color;
		petal.C1 = petal_color;
		scene.petals[i] = renderer.Add(petal);
	}

	for (int i = 0; i < RibbonLinks; ++i)
	{
		BezierData link = Cubic(
			{ 0.0f, RibbonY, 0.0f },
			{ 0.0f, RibbonY, 0.0f },
			{ 0.0f, RibbonY, 0.0f },
			{ 0.0f, RibbonY, 0.0f });
		link.C0 = ribbon_color0;
		link.C1 = ribbon_color1;
		scene.ribbon[i] = renderer.Add(link);
	}

	// The cap gallery is the one group whose caps are NOT driven by the style panel: each stroke
	// keeps the pair it was built with, so all five caps stay on screen at once while you change
	// everything else around them.
	for (int i = 0; i < GalleryCount; ++i)
	{
		BezierData stroke = Linear(
			{ 0.0f, GalleryBottom, 0.0f },
			{ 0.0f, GalleryTop, 0.0f });
		stroke.cap_front = GalleryFrontCap(i);
		stroke.cap_back = GalleryBackCap(i);
		stroke.resolution = GalleryResolution;
		stroke.C0 = { 1.0f, 1.0f, 1.0f };
		stroke.C1 = gallery_color;
		scene.gallery[i] = renderer.Add(stroke);
	}
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
			{ dx - 3.0f,         WaveY - 0.5f, 0.0f },
			{ dx - 1.5f + swing, WaveY + 0.7f, 0.0f },
			{ dx + 1.5f + swing, WaveY + 0.7f, 0.0f },
			{ dx + 3.0f,         WaveY - 0.5f, 0.0f });
		scene.wave.colors(c0, c1);
	}

	// --- petals: quadratic arcs spaced around a rotating ring, with gaps between them ------------
	for (int i = 0; i < PetalCount; ++i)
	{
		const float angle = t + (Tau * i / PetalCount);
		const float nextAngle = angle + (Tau / (2.0f * PetalCount));
		const float midAngle = (angle + nextAngle) * 0.5f;
		const float controlRadius = PetalRadius * 1.5f;

		const XMFLOAT3 P0 = { dx + PetalRadius * cosf(angle),         PetalCenterY + PetalRadius * sinf(angle),         0.0f };
		const XMFLOAT3 P2 = { dx + PetalRadius * cosf(nextAngle),     PetalCenterY + PetalRadius * sinf(nextAngle),     0.0f };
		const XMFLOAT3 P1 = { dx + controlRadius * cosf(midAngle),    PetalCenterY + controlRadius * sinf(midAngle),    0.0f };

		const XMFLOAT3 color = animate_colors ? HueColor(angle / Tau) : petal_color;

		scene.petals[i].control_points(P0, P1, P2);
		scene.petals[i].colors(color, color);
	}

	// --- ribbon: a C1-continuous chain of cubics along a 3D path ---------------------------------
	// Each link is its own curve, so the chain also shows what caps do at an interior joint: with
	// Butt the links meet seamlessly, with Round they bulge a little, and with Triangle out you get
	// a visible spike at every link boundary. That is correct - the renderer has no idea two curves
	// were meant to be one stroke.
	{
		const float step = 1.0f / RibbonLinks;

		// Sampled anywhere, including outside [0, 1], so the Catmull-Rom tangents at the ends need
		// no phantom knots.
		auto ribbon_point = [&](float s)
		{
			return XMFLOAT3{
				dx + (s * 2.0f - 1.0f) * RibbonHalfSpan,
				RibbonY + RibbonAmplitude * sinf(s * Tau * 1.5f + t * 1.3f),
				RibbonDepth * cosf(s * Tau + t * 0.7f)
			};
		};

		for (int i = 0; i < RibbonLinks; ++i)
		{
			const float s0 = i * step;
			const float s1 = (i + 1) * step;

			const XMFLOAT3 previous = ribbon_point(s0 - step);
			const XMFLOAT3 begin = ribbon_point(s0);
			const XMFLOAT3 end = ribbon_point(s1);
			const XMFLOAT3 next = ribbon_point(s1 + step);

			// Catmull-Rom knots -> cubic Bezier control points.
			const XMFLOAT3 P1 = Add3(begin, Scale3(Sub3(end, previous), 1.0f / 6.0f));
			const XMFLOAT3 P2 = Sub3(end, Scale3(Sub3(next, begin), 1.0f / 6.0f));

			XMFLOAT3 c0 = Lerp3(ribbon_color0, ribbon_color1, s0);
			XMFLOAT3 c1 = Lerp3(ribbon_color0, ribbon_color1, s1);
			if (animate_colors)
			{
				c0 = HueColor(s0 + t * 0.15f);
				c1 = HueColor(s1 + t * 0.15f);
			}

			scene.ribbon[i].control_points(begin, P1, P2, end);
			scene.ribbon[i].colors(c0, c1);
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
}

void App::ApplyStyle(Scene& scene)
{
	const auto cap_front = static_cast<CurveCap>(style_cap_front);
	const auto cap_back = static_cast<CurveCap>(link_caps ? style_cap_front : style_cap_back);

	// `target`, not `curve` - a local called `curve` would shadow nothing here today, but it does
	// in any translation unit that also names the enums through a namespace, so keep the habit.
	auto apply = [&](BezierCurve target, bool keep_own_caps, unsigned resolution)
	{
		target.Width(style_width);
		if (!keep_own_caps)
			target.Cap(cap_front, cap_back);
		target.Join(static_cast<CurveJoin>(style_join));
		target.DashLength(style_dash_length);
		// Spacing 0 is how a curve comes out solid now: pattern_ini counts no centres for it, and the
		// pixel shader leaves the body alone.
		target.Spacing(style_patterned ? style_spacing : 0.0f);
		target.HeightRange(style_min_height, style_max_height);

		// Resolution changes force a layout rebuild, so only push it when it actually moved rather
		// than every frame.
		if (target.Resolution() != resolution)
			target.Resolution(resolution);
	};

	const auto resolution = static_cast<unsigned>(style_resolution);

	apply(scene.wave, false, resolution);
	for (auto& petal : scene.petals)
		apply(petal, false, resolution);
	for (auto& link : scene.ribbon)
		apply(link, false, resolution);

	// The gallery keeps the cap pair it was built with - that is the whole point of it - and stays
	// at two points, because a straight line gains nothing from subdivision.
	for (auto& stroke : scene.gallery)
		apply(stroke, true, GalleryResolution);
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
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

	ImGui::StyleColorsDark();

	ImGui_ImplSDL3_InitForD3D(window);
	ImGui_ImplDX11_Init(device->get(), device->ImmediateContext()->get());

	camera.SetProj(viewport_width / viewport_height, 0.1f, 1000.0f);
	// Far enough back that both copies of the scene fit side by side. The orbital manipulator picks
	// its orbit distance up from this, so change it here rather than in OrbitalCamera.
	camera.SetView(XMVECTOR{ 0.0f, 0.0f, 0.0f, 0.0f }, XMVECTOR{ 0.0f, 0.0f, -14.0f, 0.0f }, XMVECTOR{ 0.0f, 1.0f, 0.0f, 0.0f });
	orbital_manipulator.SetCamera(&camera);
	buffer_camera = std::make_unique<Axodox::Graphics::ConstantBuffer>(*device, camera.GetData());

	patterned_renderer = std::make_unique<BezierRenderer>(*device);
	solid_renderer = std::make_unique<BezierSolidRenderer>(*device);

	patterned_renderer->SetViewport(viewport_width, viewport_height);
	solid_renderer->SetViewport(viewport_width, viewport_height);

	BuildScene(*patterned_renderer, patterned_scene, -compare_offset * 0.5f);
	BuildScene(*solid_renderer, solid_scene, compare_offset * 0.5f);

	PoseScene(patterned_scene);
	PoseScene(solid_scene);
	ApplyStyle(patterned_scene);
	ApplyStyle(solid_scene);

	return SDL_APP_CONTINUE;
}

void App::Update(float delta)
{
	orbital_manipulator.Update(delta);
	if (!paused)
		animation_time += delta;

	// With one renderer hidden there is nothing to compare against, so recentre the survivor
	// instead of leaving it off to one side.
	const bool side_by_side = draw_patterned && draw_solid;
	patterned_scene.x_offset = side_by_side ? -compare_offset * 0.5f : 0.0f;
	solid_scene.x_offset = side_by_side ? compare_offset * 0.5f : 0.0f;

	// Both copies are kept up to date even while hidden: the setters only raise dirty flags, and
	// the actual upload happens inside Draw(), which a hidden renderer never reaches.
	PoseScene(patterned_scene);
	PoseScene(solid_scene);
	ApplyStyle(patterned_scene);
	ApplyStyle(solid_scene);
}

void App::Gui()
{
	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(360, 660), ImGuiCond_FirstUseEver);

	if (ImGui::Begin("Curve Style"))
	{
		ImGui::Text("%.1f FPS (%.2f ms/frame)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
		ImGui::Checkbox("Pause animation", &paused);

		ImGui::SeparatorText("Renderers");
		ImGui::Checkbox("Patterned (left)", &draw_patterned);
		ImGui::SameLine();
		ImGui::Checkbox("Solid (right)", &draw_solid);
		ImGui::TextDisabled("%u curves each, %u total",
			unsigned(patterned_renderer->Count()),
			unsigned(patterned_renderer->Count() + solid_renderer->Count()));

		ImGui::BeginDisabled(!(draw_patterned && draw_solid));
		ImGui::SliderFloat("Comparison gap", &compare_offset, 0.0f, 14.0f, "%.1f world units");
		ImGui::EndDisabled();
		ImGui::TextWrapped(
			"BezierSolidRenderer draws every curve solid: it has no pattern pipeline at all, so the "
			"dash and spacing controls below only move the left copy. Untick \"Patterned\" and the "
			"two halves should look identical.");

		ImGui::SeparatorText("Stroke");
		ImGui::SliderFloat("Width (px)", &style_width, 1.0f, 40.0f, "%.1f");
		ImGui::SliderInt("Resolution", &style_resolution, 2, 400);

		ImGui::SeparatorText("Caps");
		ImGui::Checkbox("Same cap at both ends", &link_caps);
		ImGui::Combo("Front cap", &style_cap_front, CapNames, CapCount);

		// While linked the back picker mirrors the front one instead of showing a stale value.
		int shown_back_cap = link_caps ? style_cap_front : style_cap_back;
		ImGui::BeginDisabled(link_caps);
		if (ImGui::Combo("Back cap", &shown_back_cap, CapNames, CapCount))
			style_cap_back = shown_back_cap;
		ImGui::EndDisabled();
		ImGui::TextWrapped(
			"The row of straight strokes at the top ignores these and keeps one fixed pair each, so "
			"every cap stays visible. Their white end is the front.");

		ImGui::SeparatorText("Join");
		ImGui::RadioButton("Round##join", &style_join, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Square##join", &style_join, 1);

		ImGui::SeparatorText("Pattern");
		ImGui::Checkbox("Patterned", &style_patterned);
		ImGui::SameLine();
		ImGui::TextDisabled("(off = spacing 0 = solid)");

		ImGui::BeginDisabled(!style_patterned);
		ImGui::SliderFloat("Dash length (px)", &style_dash_length, 0.0f, 200.0f, "%.1f");
		ImGui::SliderFloat("Spacing", &style_spacing, 0.05f, 2.0f, "%.2f world units");

		// A dot is not a mode: it is a zero-length dash whose two round caps land on top of each
		// other, leaving a disc of radius `width`. This button just sets those three values.
		if (ImGui::Button("Make dots"))
		{
			style_dash_length = 0.0f;
			style_cap_front = int(CurveCap::Round);
			style_cap_back = int(CurveCap::Round);
			link_caps = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("dash length 0 + round caps");
		ImGui::EndDisabled();
		ImGui::TextWrapped(
			"Dash length is the pixel length of one dash, cap to cap; both of its ends wear the cap "
			"chosen above, so 0 draws nothing unless that cap is Round. The cap gallery keeps its own "
			"caps, which is why its strokes dash differently from everything else.");

		ImGui::SeparatorText("Colour");
		ImGui::Checkbox("Animate colours", &animate_colors);
		ImGui::BeginDisabled(animate_colors);
		ImGui::ColorEdit3("Wave A", &wave_color0.x);
		ImGui::ColorEdit3("Wave B", &wave_color1.x);
		ImGui::ColorEdit3("Petals", &petal_color.x);
		ImGui::ColorEdit3("Ribbon A", &ribbon_color0.x);
		ImGui::ColorEdit3("Ribbon B", &ribbon_color1.x);
		ImGui::ColorEdit3("Gallery", &gallery_color.x);
		ImGui::EndDisabled();

		ImGui::SeparatorText("Colour by height");
		ImGui::TextWrapped("Min >= max blends by curve t instead of world Y.");
		ImGui::SliderFloat("Min height", &style_min_height, -5.0f, 5.0f, "%.2f");
		ImGui::SliderFloat("Max height", &style_max_height, -5.0f, 5.0f, "%.2f");
	}

	ImGui::End();
}

void App::Render()
{
	buffer_camera->Bind(Axodox::Graphics::ShaderStage::Vertex, 0);
	buffer_camera->Bind(Axodox::Graphics::ShaderStage::Pixel, 0);

	const auto view_proj = camera.GetViewProj();
	if (draw_patterned) patterned_renderer->Draw(*device, view_proj);
	if (draw_solid) solid_renderer->Draw(*device, view_proj);
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
			orbital_manipulator.KeyboardDown(Event.key);
		if (Event.type == SDL_EVENT_KEY_UP)
			orbital_manipulator.KeyboardUp(Event.key);
	}
	if (!io.WantCaptureMouse)
	{
		if (Event.type == SDL_EVENT_MOUSE_MOTION)
			orbital_manipulator.MouseMove(Event.motion);
		if (Event.type == SDL_EVENT_MOUSE_WHEEL)
			orbital_manipulator.MouseWheel(Event.wheel);
	}
	if (Event.type == SDL_EVENT_WINDOW_RESIZED) {
		viewport_width = float(Event.window.data1);
		viewport_height = float(Event.window.data2);

		camera.SetAspect(viewport_width / viewport_height);
		patterned_renderer->SetViewport(viewport_width, viewport_height);
		solid_renderer->SetViewport(viewport_width, viewport_height);
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
