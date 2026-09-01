#pragma once
#include <SDL3/SDL.h>
#include <Include/Axodox.Graphics.h>
#include "orbital_camera.h"
#include "pipeline.h"
#include "bezier_simple.h"

class App
{
private:
	SDL_Window* window;
	std::unique_ptr<Axodox::Graphics::GraphicsDevice> device;
	std::unique_ptr<Axodox::Graphics::HwndSwapChain> swapchain;
	std::unique_ptr<Axodox::Graphics::DepthStencil2D> depth;
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> buffer_camera;

	std::unique_ptr<BezierRenderer> bezier;

	Camera camera;
	OrbitalCamera orbital_manipulator;

	float viewport_width = 640.0f;
	float viewport_height = 480.0f;

	int current_example = 0;
	int active_bezier_example = -1;
	float animation_time = 0.0f;

	int pattern_style = 0;
	float dash_spacing = 0.35f;
	float dash_duty = 0.55f;

	// Which pattern implementation the renderer runs, plus the glyph settings the second one uses.
	int pattern_mode = 0; // 0 = arc dash, 1 = glyph
	int glyph_kind = 0;   // GlyphKind
	bool glyph_world_sized = true;
	float glyph_size_world = 0.18f; // half-extent, world units
	float glyph_size_px = 9.0f;     // half-extent, pixels

	void Update(float delta);
	void Gui();
	void Render();

	void BuildBezierExample_SimpleCubicCurve();
	void BuildBezierExample_MultipleStyledCurves();
	void BuildBezierExample_LinearAndQuadratic();
	void BuildBezierExample_3DSpiral();
	void BuildBezierExample_AnimatedCurves();
	void BuildBezierExample_CurveGrid();
	void BuildBezierExample_ColoredGradients();
	void BuildBezierExample_BatchedCurves();

	// Rebuilds bezier_final's curve data only when current_example changed (or
	// every frame for the animated example, since its geometry depends on
	// animation_time), and keeps its viewport in sync with viewport_width/height.
	void UpdateBezierExample();

	friend class GUI;
public:
	SDL_AppResult Init();
	SDL_AppResult Iterate(float delta);
	SDL_AppResult Event(const SDL_Event& Event);
	void Quit(SDL_AppResult result);
};
