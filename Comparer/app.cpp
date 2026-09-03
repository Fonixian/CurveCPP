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

void App::BuildScene()
{
	// Initial pose - AnimateScene() overwrites every field that depends on
	// animation_time on the very first frame, so these values only matter until then.
	curve::BezierData wave = curve::Cubic(
		{ -3.0f, -1.0f, 0.0f },
		{ -1.5f,  0.5f, 0.0f },
		{  1.5f,  0.5f, 0.0f },
		{  3.0f, -1.0f, 0.0f });
	wave.C0 = wave_color0;
	wave.C1 = wave_color1;
	wave_curve = bezier->Add(wave);

	for (int i = 0; i < PetalCount; ++i)
	{
		curve::BezierData petal = curve::Cubic(
			{ 1.5f, 0.0f, 0.0f },
			{ 1.5f, 0.0f, 0.0f },
			{ 1.5f, 0.0f, 0.0f },
			{ 1.5f, 0.0f, 0.0f });
		petal.C0 = petal_color;
		petal.C1 = petal_color;
		petal_curves[i] = bezier->Add(petal);
	}
}

void App::AnimateScene()
{
	float offset = sinf(animation_time * 2.0f) * 1.0f;

	XMFLOAT3 c0 = wave_color0;
	XMFLOAT3 c1 = wave_color1;
	if (animate_colors)
	{
		c0 = { 1.0f - animation_time / 10.0f, 0.5f, 1.0f };
		c1 = { 0.2f, 1.0f, 0.5f - animation_time / 10.0f };
	}

	wave_curve.control_points({ -3.0f, -1.0f, 0.0f }, { -1.5f + offset, 0.5f, 0.0f }, { 1.5f + offset, 0.5f, 0.0f }, { 3.0f, -1.0f, 0.0f });
	wave_curve.colors(c0,c1);

	float rotation = animation_time;
	for (int i = 0; i < PetalCount; ++i)
	{
		float angle = rotation + (2.0f * 3.14159f * i / PetalCount);
		float radius = 1.5f;
		float nextAngle = angle + (2.0f * 3.14159f / (2.0f * PetalCount));

		XMFLOAT3 P0 = { radius * cosf(angle),     radius * sinf(angle),     0.0f };
		XMFLOAT3 P3 = { radius * cosf(nextAngle), radius * sinf(nextAngle), 0.0f };

		float controlRadius = radius * 1.5f;
		float midAngle = angle + (nextAngle - angle) / 2.0f;
		XMFLOAT3 P1 = { controlRadius * cosf(midAngle), controlRadius * sinf(midAngle), 0.0f };

		XMFLOAT3 color = petal_color;
		if (animate_colors)
		{
			float hue = angle / 6.28f;
			color = {
				cosf(hue * 3.14159f),
				cosf(hue * 3.14159f + 2.09f),
				cosf(hue * 3.14159f + 4.19f)
			};
		}

		petal_curves[i].control_points(P0, P1, P1, P3);
		petal_curves[i].colors(color,color);
	}
}

void App::ApplyStyle()
{
	auto apply = [&](curve::BezierCurve target)
	{
		target.Width(style_width);
		target.Cap(static_cast<curve::CurveCap>(style_cap));
		target.Join(static_cast<curve::CurveJoin>(style_join));
		target.Pattern(static_cast<curve::CurvePattern>(style_pattern));
		target.Spacing(style_spacing);
		target.HeightRange(style_min_height, style_max_height);

		// Resolution changes force a layout rebuild, so only push it when it actually
		// moved rather than every frame.
		if (target.Resolution() != static_cast<unsigned>(style_resolution))
			target.Resolution(static_cast<unsigned>(style_resolution));
	};

	apply(wave_curve);
	for (auto& petal : petal_curves)
		apply(petal);
}

SDL_AppResult App::Init()
{
	SDL_Init(SDL_INIT_VIDEO);
	window = SDL_CreateWindow("Transparency Comparison", 640, 480, SDL_WINDOW_RESIZABLE);
	if (window == NULL)
	{
		return SDL_APP_FAILURE;
	}
	HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

	viewport_width = 640.0f;
	viewport_height = 480.0f;

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
	camera.SetView(XMVECTOR{ 0.0f, 0.0f, 0.0f, 0.0f }, XMVECTOR{ 0.0f, 0.0f, -8.0f, 0.0f }, XMVECTOR{ 0.0f, 1.0f, 0.0f, 0.0f });
	orbital_manipulator.SetCamera(&camera);
	buffer_camera = std::make_unique<Axodox::Graphics::ConstantBuffer>(*device, camera.GetData());

	bezier = std::make_unique<curve::BezierRenderer>(*device);
	bezier->SetViewport(viewport_width, viewport_height);
	BuildScene();
	ApplyStyle();
	AnimateScene();

	return SDL_APP_CONTINUE;
}

void App::Update(float delta)
{
	orbital_manipulator.Update(delta);
	if (!paused)
		animation_time += delta;

	AnimateScene();
	ApplyStyle();
}

void App::Gui()
{
	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(340, 480), ImGuiCond_FirstUseEver);

	if (ImGui::Begin("Curve Style"))
	{
		ImGui::Checkbox("Pause animation", &paused);

		ImGui::Separator();
		ImGui::Text("Stroke");
		ImGui::SliderFloat("Width (px)", &style_width, 1.0f, 40.0f, "%.1f");
		ImGui::SliderInt("Resolution", &style_resolution, 2, 400);

		ImGui::Separator();
		ImGui::Text("Cap");
		ImGui::RadioButton("Butt", &style_cap, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Square", &style_cap, 1);
		ImGui::SameLine();
		ImGui::RadioButton("Round", &style_cap, 2);
		ImGui::RadioButton("Triangle out", &style_cap, 3);
		ImGui::SameLine();
		ImGui::RadioButton("Triangle in", &style_cap, 4);

		ImGui::Separator();
		ImGui::Text("Join");
		ImGui::RadioButton("Round##join", &style_join, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Square##join", &style_join, 1);

		ImGui::Separator();
		ImGui::Text("Pattern");
		ImGui::RadioButton("Solid", &style_pattern, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Dash", &style_pattern, 1);
		ImGui::SameLine();
		ImGui::RadioButton("Dot", &style_pattern, 2);

		if (style_pattern != 0)
			ImGui::SliderFloat("Spacing", &style_spacing, 0.05f, 2.0f, "%.2f world units");

		ImGui::Separator();
		ImGui::Text("Colour");
		ImGui::Checkbox("Animate colours", &animate_colors);
		ImGui::BeginDisabled(animate_colors);
		ImGui::ColorEdit3("Wave colour A", &wave_color0.x);
		ImGui::ColorEdit3("Wave colour B", &wave_color1.x);
		ImGui::ColorEdit3("Petal colour", &petal_color.x);
		ImGui::EndDisabled();

		ImGui::Separator();
		ImGui::TextWrapped("Colour-by-height band (min >= max blends by curve t instead):");
		ImGui::SliderFloat("Min height", &style_min_height, -3.0f, 3.0f, "%.2f");
		ImGui::SliderFloat("Max height", &style_max_height, -3.0f, 3.0f, "%.2f");
	}

	ImGui::End();
}

void App::Render()
{
	buffer_camera->Bind(Axodox::Graphics::ShaderStage::Vertex, 0);
	buffer_camera->Bind(Axodox::Graphics::ShaderStage::Pixel, 0);
	bezier->Draw(*device, camera.GetViewProj());
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
		bezier->SetViewport(viewport_width, viewport_height);
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
