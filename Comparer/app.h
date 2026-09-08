#pragma once
#include <SDL3/SDL.h>
#include <Include/Axodox.Graphics.h>
#include "orbital_camera.h"
#include "bezier.h"
#include "bezier_solid.h"

class App
{
private:
	SDL_Window* window;
	std::unique_ptr<Axodox::Graphics::GraphicsDevice> device;
	std::unique_ptr<Axodox::Graphics::HwndSwapChain> swapchain;
	std::unique_ptr<Axodox::Graphics::DepthStencil2D> depth;
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> buffer_camera;

	// Both renderers are alive at once and each owns its own copy of the same scene, drawn side by
	// side: the patterned one on the left, the solid-only one on the right. That is the point of
	// this app - with the pattern set to Solid the two halves should be pixel-identical, and any
	// difference in cost between them is the price of the pattern pipeline.
	//
	// A curve belongs to the renderer that created it and cannot be moved between them, so the
	// scene really is built twice.
	std::unique_ptr<BezierRenderer> patterned_renderer;
	std::unique_ptr<BezierSolidRenderer> solid_renderer;

	Camera camera;
	OrbitalCamera orbital_manipulator;

	float viewport_width = 1280.0f;
	float viewport_height = 720.0f;

	float animation_time = 0.0f;
	bool paused = false;

	static constexpr int PetalCount = 5;    // arcs chasing each other around a rotating ring
	static constexpr int RibbonLinks = 6;   // C1-continuous chain of cubics, waving through Z
	static constexpr int GalleryCount = 5;  // one straight stroke per CurveCap value

	// A straight line needs no subdivision, and one segment per gallery stroke means it has exactly
	// two ends and no joins - which is what makes it a clean cap reference.
	static constexpr unsigned GalleryResolution = 2u;

	// One renderer's worth of scene. Curves are added once in BuildScene() and never removed
	// (neither renderer has a Clear()), so animation reposes them in place through these handles
	// every frame instead. x_offset slides the whole copy sideways so the two can be compared.
	struct Scene
	{
		float x_offset = 0.0f;
		BezierCurve wave;
		BezierCurve petals[PetalCount];
		BezierCurve ribbon[RibbonLinks];
		BezierCurve gallery[GalleryCount];
	};

	Scene patterned_scene;
	Scene solid_scene;

	// --- what gets drawn -----------------------------------------------------------
	bool draw_patterned = true;
	bool draw_solid = true;
	// World-space gap between the two copies. Only used while both are visible; with one of them
	// hidden the survivor is recentred on the origin instead.
	float compare_offset = 8.0f;

	// --- runtime-editable style, applied to every curve in the scene every frame ---
	float style_width = 10.0f;      // half-width, in pixels
	int style_cap_front = 2;        // CurveCap at the P0 end
	int style_cap_back = 0;         // CurveCap at the far end
	bool link_caps = false;         // drive both ends from the front picker
	int style_join = 0;             // CurveJoin
	// The pattern is no longer an enum. `patterned` off uploads spacing 0, which produces no pattern
	// centres and so a solid stroke - that is what makes the two halves comparable. A dot is
	// style_dash_length 0 with both caps set to Round; the "Dot" button in the panel does exactly
	// that and nothing else.
	bool style_patterned = true;     // off => spacing 0 => solid
	float style_dash_length = 80.0f; // LENGTH of one dash, in pixels (0 + round caps = dot)
	float style_spacing = 0.35f;     // world units between pattern centers
	int style_resolution = 120;      // sample points per curve
	float style_min_height = 0.0f;  // colour-by-height band; min >= max blends by curve t instead
	float style_max_height = 0.0f;

	bool animate_colors = true;
	DirectX::XMFLOAT3 wave_color0 = { 1.0f, 0.5f, 1.0f };
	DirectX::XMFLOAT3 wave_color1 = { 0.2f, 1.0f, 0.5f };
	DirectX::XMFLOAT3 petal_color = { 1.0f, 0.6f, 0.2f };
	DirectX::XMFLOAT3 ribbon_color0 = { 0.35f, 0.75f, 1.0f };
	DirectX::XMFLOAT3 ribbon_color1 = { 0.95f, 0.35f, 0.75f };
	DirectX::XMFLOAT3 gallery_color = { 1.0f, 0.85f, 0.25f };

	void Update(float delta);
	void Gui();
	void Render();

	// Adds one copy of the scene to `renderer` and fills `scene` with handles to it.
	void BuildScene(BezierRendererBase& renderer, Scene& scene, float x_offset);
	// Re-poses and re-colours one copy for the current animation_time and x_offset.
	void PoseScene(Scene& scene);
	// Pushes the runtime style controls onto one copy. The gallery keeps its own caps.
	void ApplyStyle(Scene& scene);

public:
	SDL_AppResult Init();
	SDL_AppResult Iterate(float delta);
	SDL_AppResult Event(const SDL_Event& Event);
	void Quit(SDL_AppResult result);
};
