#pragma once
#include <SDL3/SDL.h>
#include <Include/Axodox.Graphics.h>
#include "orbital_camera.h"
#include "bezier.h"
#include "bezier_solid.h"
#include "bezier_dots.h"
#include <vector>

class App
{
private:
	SDL_Window* window;
	std::unique_ptr<Axodox::Graphics::GraphicsDevice> device;
	std::unique_ptr<Axodox::Graphics::HwndSwapChain> swapchain;
	std::unique_ptr<Axodox::Graphics::DepthStencil2D> depth;
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> buffer_camera;

	// All three renderers are alive at once and each owns its own copy of the same scene, drawn side
	// by side: patterned, then solid, then dots. That is the point of this app - with the pattern set
	// to Solid the first two halves should be pixel-identical, and any difference in cost between them
	// is the price of the pattern pipeline; the third is BezierDotRenderer, which draws only dot
	// patterns via its own instanced-quad technique (see bezier_dots.h) rather than piggybacking dots
	// on a line-strip's SDF the way the patterned renderer's dash_length == 0 case does.
	//
	// A curve belongs to the renderer that created it and cannot be moved between them, so the
	// scene really is built three times.
	std::unique_ptr<BezierRenderer> patterned_renderer;
	std::unique_ptr<BezierSolidRenderer> solid_renderer;
	std::unique_ptr<BezierDotRenderer> dot_renderer;

	Camera camera;
	OrbitalCamera orbital_manipulator;

	float viewport_width = 1600.0f;
	float viewport_height = 800.0f;

	float animation_time = 0.0f;
	bool paused = false;

	static constexpr int PetalCount = 5;    // arcs chasing each other around a rotating ring
	static constexpr int RibbonLinks = 6;   // C1-continuous chain of cubics, waving through Z
	static constexpr int GalleryCount = 5;  // one straight stroke per CurveCap value

	// A straight line needs no subdivision, and one segment per gallery stroke means it has exactly
	// two ends and no joins - which is what makes it a clean cap reference.
	static constexpr unsigned GalleryResolution = 2u;

	// One curve spawned by the Test screen. Its control points are stored in the copy's LOCAL
	// coordinates - x_offset is added back in PoseScene() - so a test curve keeps following its own
	// column when the layout shifts (a renderer being ticked on or off, or the comparison gap moving).
	// Resolution is per curve rather than per style: it is chosen at Add time and is exactly what the
	// test is varying, so the Stroke section's global Resolution slider deliberately does not touch it.
	struct TestCurve
	{
		BezierCurve handle;
		DirectX::XMFLOAT3 P[4] = {};  // only the first `degree + 1` are meaningful
		int degree = 3;               // 1 linear, 2 quadratic, 3 cubic
		unsigned resolution = 120u;
	};

	// One renderer's worth of scene. The four example groups are added once in BuildScene() and never
	// removed (none of the three renderers has a Clear()), so animation reposes them in place through
	// these handles every frame instead. x_offset slides the whole copy sideways so the copies can be
	// compared; BezierCurve itself is renderer-agnostic, so the same Scene type serves all three.
	//
	// test_curves grows from the Test screen and, for the same reason, never shrinks.
	struct Scene
	{
		float x_offset = 0.0f;
		BezierCurve wave;
		BezierCurve petals[PetalCount];
		BezierCurve ribbon[RibbonLinks];
		BezierCurve gallery[GalleryCount];
		std::vector<TestCurve> test_curves;
	};

	Scene patterned_scene;
	Scene solid_scene;
	Scene dot_scene;

	// --- screens -------------------------------------------------------------------
	// Two tabs in the control window. Example is the hand-built demo scene; Test spawns random curves
	// into one renderer at a time so the Timings window has something to measure. Both screens share
	// the renderer checkboxes, the style controls, and the same three scenes - the tab only decides
	// which controls are on screen, never what is drawn.
	bool show_timings = true;

	// Set by anything that changes what PoseScene() / ApplyStyle() would produce. Both passes touch()
	// every curve they visit, which raises need_upload and so re-uploads the WHOLE curve buffer, so
	// running them unconditionally every frame made "Pause animation" cost exactly as much as playing
	// it. With a scene of a few thousand test curves that upload is the measurement, so both are now
	// gated: paused really means idle, and the Timings window then shows the draw cost alone.
	bool scene_dirty = true;
	bool style_dirty = true;

	// --- Test screen ---------------------------------------------------------------
	int test_degree_index = 2;      // 0 linear, 1 quadratic, 2 cubic
	int test_count = 50;            // curves added per button press
	int test_resolution = 120;      // sample points per added curve, kept per curve afterwards
	// Half-extents of the box a new curve's origin lands in, in one copy's local coordinates. The
	// example scene occupies x in [-3, 3], y in [-4.4, 4.6], so the defaults keep test curves roughly
	// inside their own column rather than sprawling across the neighbouring one.
	DirectX::XMFLOAT3 test_range = { 2.5f, 3.5f, 1.5f };
	// How far the individual control points scatter from that origin. Small values give short, tame
	// strokes; large ones give scribbles that span the box.
	float test_spread = 0.8f;

	// --- what gets drawn -----------------------------------------------------------
	bool draw_patterned = true;
	bool draw_solid = true;
	bool draw_dots = true;
	// World-space gap between adjacent visible copies, evenly spaced and centred on the origin -
	// see Update() for how 1, 2 or 3 visible copies are laid out.
	float compare_offset = 7.0f;

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
	// The Example tab: what the demo scene is, plus the per-group colour pickers.
	void ExampleGui();
	// The Test tab: the random-curve spawner and one Add button per renderer.
	void TestGui();
	// Stroke / caps / join / pattern / height band. Shown on both tabs because it drives every curve
	// in every scene, test curves included.
	void StyleGui();
	// The timings window. Reads each renderer's public Profiler, one column per renderer.
	void ProfilerGui();
	void Render();

	// Adds one copy of the scene to `renderer` and fills `scene` with handles to it.
	void BuildScene(BezierRendererBase& renderer, Scene& scene, float x_offset);
	// Spawns test_count random curves into ONE renderer and records them in that renderer's scene.
	// Deliberately per renderer rather than broadcast: the scenes are allowed to diverge, which is the
	// only way to give one renderer a heavier load than the others and watch the Timings table split.
	// `force_spacing` matches ApplyStyle()'s parameter - see there.
	void AddTestCurves(BezierRendererBase& renderer, Scene& scene, bool force_spacing);
	// Re-poses and re-colours one copy for the current animation_time and x_offset.
	void PoseScene(Scene& scene);
	// Pushes the runtime style controls onto one copy. The gallery keeps its own caps. `force_spacing`
	// is for dot_scene: BezierDotRenderer has no solid/off state, so its spacing must not go to 0 just
	// because the "Patterned" checkbox (which only means something for the first two renderers) is
	// unticked.
	void ApplyStyle(Scene& scene, bool force_spacing = false);

public:
	SDL_AppResult Init();
	SDL_AppResult Iterate(float delta);
	SDL_AppResult Event(const SDL_Event& Event);
	void Quit(SDL_AppResult result);
};
