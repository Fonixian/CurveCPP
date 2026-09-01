#include "app.h"
#include <d3d11.h>
#include <DirectXMath.h>
#include <imgui.h>
#include "imgui_impl_sdl3.h"
#include "imgui_impl_dx11.h"
#include "Include/Axodox.Storage.h"
#include <iostream>

using namespace Axodox::Storage;
using namespace Axodox::Graphics;
using namespace DirectX;

void App::BuildBezierExample_SimpleCubicCurve()
{
	bezier->Clear();

	bezier->Add(cubic(
		{ -3.0f, -1.0f, 0.0f },
		{ -1.5f,  2.0f, 0.0f },
		{  1.5f,  2.0f, 0.0f },
		{  3.0f, -1.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f },
		2.0f, PatternStyle::Solid, 2.0f, 100
	));
}

void App::BuildBezierExample_MultipleStyledCurves()
{
	bezier->Clear();

	bezier->Add(cubic(
		{ -3.5f, -1.8f, 0.0f },
		{ -2.0f,  0.0f, 0.0f },
		{  2.0f, -0.6f, 0.0f },
		{  3.5f, -2.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f },
		{ 1.0f, 0.5f, 0.0f },
		5.5f, PatternStyle::Segmented, 1.0f, 150
	));

	bezier->Add(cubic(
		{ -3.5f, -0.4f, 0.0f },
		{ -2.0f,  1.4f, 0.0f },
		{  2.0f,  0.8f, 0.0f },
		{  3.5f, -0.6f, 0.0f },
		{ 0.0f, 1.0f, 0.0f },
		{ 0.0f, 0.5f, 1.0f },
		2.0f, PatternStyle::Segmented, 1.0f, 150
	));

	bezier->Add(cubic(
		{ -3.5f, 1.0f, 0.0f },
		{ -2.0f, 2.6f, 0.0f },
		{  2.0f, 2.0f, 0.0f },
		{  3.5f, 0.8f, 0.0f },
		{ 0.0f, 0.0f, 1.0f },
		{ 1.0f, 0.0f, 1.0f },
		1.5f, PatternStyle::Solid, 2.0f, 100
	));
}

void App::BuildBezierExample_LinearAndQuadratic()
{
	bezier->Clear();

	bezier->Add(linear(
		{ -3.5f, -1.5f, 0.0f },
		{ -0.5f,  1.0f, 0.0f },
		{ 1.0f, 1.0f, 0.0f },
		{ 0.0f, 1.0f, 1.0f },
		2.0f, PatternStyle::Solid, 2.0f, 50
	));

	bezier->Add(quadratic(
		{ 0.5f, -1.5f, 0.0f },
		{ 2.0f,  2.5f, 0.0f },
		{ 3.5f, -1.5f, 0.0f },
		{ 1.0f, 0.0f, 1.0f },
		{ 0.5f, 0.5f, 0.5f },
		2.5f, PatternStyle::Solid, 2.0f, 80
	));
}

void App::BuildBezierExample_3DSpiral()
{
	bezier->Clear();

	// Spiral around the origin (the camera's orbit centre), rising through z.
	const float radius = 2.0f;
	const float centerX = 0.0f;
	const float centerY = 0.0f;
	const float zStart = -2.0f;
	const float zPerSegment = 0.5f;
	const int numSegments = 8;

	for (int i = 0; i < numSegments; ++i)
	{
		float angle1 = (2.0f * 3.14159f * i) / numSegments;
		float angle2 = (2.0f * 3.14159f * (i + 1)) / numSegments;
		float z1 = zStart + i * zPerSegment;
		float z2 = zStart + (i + 1) * zPerSegment;

		float x1 = centerX + radius * cosf(angle1);
		float y1 = centerY + radius * sinf(angle1);
		float x2 = centerX + radius * cosf(angle2);
		float y2 = centerY + radius * sinf(angle2);

		XMFLOAT3 P0 = { x1, y1, z1 };
		XMFLOAT3 P3 = { x2, y2, z2 };

		float controlRadius = radius * 1.2f;
		float controlAngle = angle1 + (angle2 - angle1) / 2.0f;
		XMFLOAT3 P1 = {
			centerX + controlRadius * cosf(controlAngle),
			centerY + controlRadius * sinf(controlAngle),
			(z1 + z2) / 2.0f - zPerSegment * 0.5f
		};
		XMFLOAT3 P2 = {
			centerX + controlRadius * cosf(controlAngle),
			centerY + controlRadius * sinf(controlAngle),
			(z1 + z2) / 2.0f + zPerSegment * 0.5f
		};

		float t = static_cast<float>(i) / numSegments;
		XMFLOAT3 colorStart = { 1.0f - t, t, 0.5f };
		XMFLOAT3 colorEnd = { t, 1.0f - t, 0.5f };

		bezier->Add(cubic(P0, P1, P2, P3, colorStart, colorEnd, 2.0f, PatternStyle::Solid, 2.0f, 60));
	}
}

void App::BuildBezierExample_AnimatedCurves()
{
	bezier->Clear();

	float offset = sinf(animation_time * 2.0f) * 1.0f;

	bezier->Add(cubic(
		{ -3.0f, -1.0f, 0.0f },
		{ -1.5f + offset, 0.5f, 0.0f },
		{  1.5f + offset, 0.5f, 0.0f },
		{  3.0f, -1.0f, 0.0f },
		{ 1.0f - animation_time / 10.0f, 0.5f, 1.0f },
		{ 0.2f, 1.0f, 0.5f - animation_time / 10.0f },
		2.5f, PatternStyle::Solid, 2.0f, 120
	));

	float rotation = animation_time;
	for (int i = 0; i < 3; ++i)
	{
		float angle = rotation + (2.0f * 3.14159f * i / 3.0f);
		float radius = 1.5f;
		float nextAngle = angle + (2.0f * 3.14159f / 6.0f);

		XMFLOAT3 P0 = {
			radius * cosf(angle),
			radius * sinf(angle),
			0.0f
		};
		XMFLOAT3 P3 = {
			radius * cosf(nextAngle),
			radius * sinf(nextAngle),
			0.0f
		};

		float controlRadius = radius * 1.5f;
		float midAngle = angle + (nextAngle - angle) / 2.0f;
		XMFLOAT3 P1 = {
			controlRadius * cosf(midAngle),
			controlRadius * sinf(midAngle),
			0.0f
		};
		XMFLOAT3 P2 = P1;

		float hue = (angle / 6.28f);
		XMFLOAT3 color = {
			cosf(hue * 3.14159f),
			cosf(hue * 3.14159f + 2.09f),
			cosf(hue * 3.14159f + 4.19f)
		};

		bezier->Add(cubic(P0, P1, P2, P3, color, color, 2.0f, PatternStyle::Solid, 2.0f, 80));
	}
}

void App::BuildBezierExample_CurveGrid()
{
	bezier->Clear();

	const int gridSize = 5;
	const float gridSpacing = 1.2f;
	const float curveHeight = 0.4f;
	// Centre the grid on the origin so the whole thing sits in front of the camera.
	const float gridOrigin = -0.5f * (gridSize - 1) * gridSpacing;

	for (int row = 0; row < gridSize; ++row)
	{
		for (int col = 0; col < gridSize; ++col)
		{
			float startX = gridOrigin + col * gridSpacing;
			float startY = gridOrigin + row * gridSpacing;
			float endX = startX + gridSpacing * 0.8f;
			float endY = startY;

			float archHeight = curveHeight * (1.0f + (row + col) / (gridSize * 2.0f));

			XMFLOAT3 P0 = { startX, startY, 0.0f };
			XMFLOAT3 P1 = { startX + (endX - startX) * 0.25f, startY - archHeight, 0.0f };
			XMFLOAT3 P2 = { startX + (endX - startX) * 0.75f, startY - archHeight, 0.0f };
			XMFLOAT3 P3 = { endX, endY, 0.0f };

			float r = static_cast<float>(col) / gridSize;
			float g = static_cast<float>(row) / gridSize;
			float b = 1.0f - (r + g) / 2.0f;

			bezier->Add(cubic(P0, P1, P2, P3, { r, g, b }, { b, r, g }, 1.5f, PatternStyle::Solid, 2.0f, 60));
		}
	}
}

void App::BuildBezierExample_ColoredGradients()
{
	bezier->Clear();

	// Three arches side by side, each 2.4 units wide, centred on the origin.
	for (int i = 0; i < 3; ++i)
	{
		const float x0 = -3.6f + i * 2.6f;

		XMFLOAT3 c0, c1;
		switch (i)
		{
		case 0:  c0 = { 1.0f, 0.0f, 0.0f }; c1 = { 0.0f, 0.0f, 1.0f }; break;
		case 1:  c0 = { 0.0f, 0.0f, 1.0f }; c1 = { 1.0f, 0.0f, 0.0f }; break;
		default: c0 = { 0.0f, 0.0f, 0.0f }; c1 = { 1.0f, 1.0f, 1.0f }; break;
		}

		bezier->Add(cubic(
			{ x0,         -0.8f, 0.0f },
			{ x0 + 0.6f,   1.2f, 0.0f },
			{ x0 + 1.8f,   1.2f, 0.0f },
			{ x0 + 2.4f,  -0.8f, 0.0f },
			c0, c1,
			3.0f, PatternStyle::Solid, 2.0f, 150
		));
	}
}

void App::BuildBezierExample_BatchedCurves()
{
	bezier->Clear();

	// 10 x 10 grid of little arcs, spanning roughly [-4, +4] on both axes.
	const float cell = 0.65f;
	const float gridOrigin = -0.5f * 9 * cell;

	for (int i = 0; i < 100; ++i)
	{
		float x = gridOrigin + (i % 10) * cell;
		float y = gridOrigin + (i / 10) * cell;

		bezier->Add(cubic(
			{ x,                y,               0.0f },
			{ x + cell * 0.25f, y + cell * 0.5f, 0.0f },
			{ x + cell * 0.55f, y + cell * 0.5f, 0.0f },
			{ x + cell * 0.75f, y,               0.0f },
			{ i / 100.0f, 0.5f, 1.0f - i / 100.0f },
			{ 1.0f - i / 100.0f, 0.5f, i / 100.0f },
			1.5f + (i % 5) * 0.5f,
			PatternStyle::Solid,
			2.0f,
			80 + (i % 3) * 20
		));
	}
}

void App::UpdateBezierExample()
{
	bool selection_changed = current_example != active_bezier_example;
	bool is_animated = current_example == 4;

	// Only re-upload curve data when the selected example changes, plus every
	// frame for the animated example (its geometry depends on animation_time).
	// Every other example is static, so redoing Clear()+Add*() for it each
	// frame would just be a wasted GPU rebuild.
	if (!selection_changed && !is_animated)
		return;

	switch (current_example)
	{
	case 0:
		BuildBezierExample_SimpleCubicCurve();
		break;
	case 1:
		BuildBezierExample_MultipleStyledCurves();
		break;
	case 2:
		BuildBezierExample_LinearAndQuadratic();
		break;
	case 3:
		BuildBezierExample_3DSpiral();
		break;
	case 4:
		BuildBezierExample_AnimatedCurves();
		break;
	case 5:
		BuildBezierExample_CurveGrid();
		break;
	case 6:
		BuildBezierExample_ColoredGradients();
		break;
	case 7:
		BuildBezierExample_BatchedCurves();
		break;
	default:
		BuildBezierExample_SimpleCubicCurve();
		break;
	}

	active_bezier_example = current_example;
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

	
	bezier = std::make_unique<BezierRenderer>(*device);
	bezier->SetViewport(viewport_width, viewport_height);
	UpdateBezierExample();

	return SDL_APP_CONTINUE;
}

void App::Update(float delta)
{
	orbital_manipulator.Update(delta);
	animation_time += delta;

	UpdateBezierExample();

	// Both are no-ops when nothing changed, so calling them every frame is free.
	bezier->SetPatternMode(pattern_mode == 1 ? PatternMode::Glyph : PatternMode::ArcDash);
	bezier->SetGlyph(static_cast<GlyphKind>(glyph_kind),
		glyph_world_sized ? glyph_size_world : glyph_size_px,
		glyph_world_sized);
}

void App::Gui()
{
	ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);

	if (ImGui::Begin("BezierRendererFinal Examples"))
	{
		ImGui::Text("Select an example:");
		ImGui::Separator();

		int previous_example = current_example;

		ImGui::RadioButton("1: Simple Cubic Curve", &current_example, 0);
		ImGui::RadioButton("2: Multiple Styled Curves", &current_example, 1);
		ImGui::RadioButton("3: Linear & Quadratic", &current_example, 2);
		ImGui::RadioButton("4: 3D Spiral", &current_example, 3);
		ImGui::RadioButton("5: Animated Curves", &current_example, 4);
		ImGui::RadioButton("6: Curve Grid", &current_example, 5);
		ImGui::RadioButton("7: Color Gradients", &current_example, 6);
		ImGui::RadioButton("8: Batched Curves (100)", &current_example, 7);

		ImGui::Separator();
		ImGui::Text("Fill pattern:");
		ImGui::RadioButton("Solid", &pattern_style, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Segmented", &pattern_style, 1);

		if (pattern_style == 1)
		{
			ImGui::SliderFloat("Dash spacing", &dash_spacing, 0.05f, 2.0f, "%.2f world units");
			ImGui::SliderFloat("Dash duty", &dash_duty, 0.05f, 1.0f, "%.2f");
		}

		ImGui::Separator();
		ImGui::Text("Pattern implementation:");
		ImGui::RadioButton("Arc dash - cut along screen arc length", &pattern_mode, 0);
		ImGui::RadioButton("Glyph - rigid SDF stamp", &pattern_mode, 1);

		if (pattern_mode == 1)
		{
			ImGui::Spacing();
			ImGui::RadioButton("Dot", &glyph_kind, 0);
			ImGui::SameLine();
			ImGui::RadioButton("Arrow", &glyph_kind, 1);
			ImGui::SameLine();
			ImGui::RadioButton("Heart", &glyph_kind, 2);
			ImGui::SameLine();
			ImGui::RadioButton("Star", &glyph_kind, 3);

			ImGui::Checkbox("World-sized (foreshortens)", &glyph_world_sized);
			if (glyph_world_sized)
				ImGui::SliderFloat("Glyph size", &glyph_size_world, 0.02f, 0.80f, "%.3f world units");
			else
				ImGui::SliderFloat("Glyph size", &glyph_size_px, 1.0f, 40.0f, "%.1f px");

			ImGui::TextWrapped("Pattern spacing is baked into each example's curves. "
				"Lower it there to see a denser chain.");
		}

		if (current_example != previous_example)
			animation_time = 0.0f;

		ImGui::End();
	}

	ImGui::ShowDemoWindow();
}

void App::Render()
{
	buffer_camera->Bind(Axodox::Graphics::ShaderStage::Vertex, 0);
	buffer_camera->Bind(Axodox::Graphics::ShaderStage::Pixel, 0);
	bezier->Draw(*device,camera.GetViewProj());
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
