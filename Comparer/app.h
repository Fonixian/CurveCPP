#pragma once
#include <SDL3/SDL.h>
#include <Include/Axodox.Graphics.h>
#include "orbital_camera.h"
#include "pipeline.h"
#include "bezier.h"

class App
{
private:
	SDL_Window* window;
	std::unique_ptr<Axodox::Graphics::GraphicsDevice> device;
	std::unique_ptr<Axodox::Graphics::HwndSwapChain> swapchain;
	std::unique_ptr<Axodox::Graphics::DepthStencil2D> depth;
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> buffer_camera;

	std::unique_ptr<curve::BezierRenderer> bezier;

	Camera camera;
	OrbitalCamera orbital_manipulator;

	float viewport_width = 640.0f;
	float viewport_height = 480.0f;

	float animation_time = 0.0f;
	bool paused = false;

	// The one scene this app shows: a wavy curve plus a few curves chasing each other
	// around a rotating triangle. Curves are added once in BuildScene() and never
	// removed (curve::BezierRenderer has no Clear()) - animation reposes them in place
	// through their handles every frame instead.
	static constexpr int PetalCount = 3;
	curve::BezierCurve wave_curve;
	curve::BezierCurve petal_curves[PetalCount];

	// --- runtime-editable style, applied to every curve in the scene every frame ---
	float style_width = 10.0f;     // half-width, in pixels
	int style_cap = 0;             // curve::CurveCap
	int style_join = 0;            // curve::CurveJoin
	int style_pattern = 1;         // curve::CurvePattern (Dash by default)
	float style_spacing = 0.35f;   // world units between pattern centers
	int style_resolution = 120;    // sample points per curve
	float style_min_height = 0.0f; // colour-by-height band; min >= max blends by curve t instead
	float style_max_height = 0.0f;

	bool animate_colors = true;
	DirectX::XMFLOAT3 wave_color0 = { 1.0f, 0.5f, 1.0f };
	DirectX::XMFLOAT3 wave_color1 = { 0.2f, 1.0f, 0.5f };
	DirectX::XMFLOAT3 petal_color = { 1.0f, 0.6f, 0.2f };

	void Update(float delta);
	void Gui();
	void Render();

	// Adds the scene's curves once and keeps handles to them.
	void BuildScene();
	// Re-poses and re-colours the scene for the current animation_time.
	void AnimateScene();
	// Pushes the runtime style controls onto every curve in the scene.
	void ApplyStyle();

public:
	SDL_AppResult Init();
	SDL_AppResult Iterate(float delta);
	SDL_AppResult Event(const SDL_Event& Event);
	void Quit(SDL_AppResult result);
};
