#pragma once
#include <SDL3/SDL.h>
#include <Include/Axodox.Graphics.h>
#include "orbital_camera.h"
#include "bezier.h"
#include "bezier_line.h"
#include "catmull_rom.h"
#include <vector>

class App
{
private:
	SDL_Window* window;
	std::unique_ptr<Axodox::Graphics::GraphicsDevice> device;
	std::unique_ptr<Axodox::Graphics::HwndSwapChain> swapchain;
	std::unique_ptr<Axodox::Graphics::DepthStencil2D> depth;
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> buffer_camera;

	// Two renderers, each with its own copy of the same scene, drawn side by side:
	//   - curve_renderer, a BezierRenderer (bezier.h): every curve goes to its solid, patterned or dot
	//     backend by its spacing and dash length, and moves when the style panel changes those. The
	//     Timings window still shows the three backends one column each.
	//   - line_renderer, the hardware line primitive (1 px, colour only), as the cost floor.
	//
	// A curve belongs to the renderer that created it, so the scene is built twice. Inside the
	// BezierRenderer nothing is built per backend - its routing is the whole point.
	std::unique_ptr<BezierRenderer> curve_renderer;
	std::unique_ptr<BezierLineRenderer> line_renderer;

	Camera camera;
	OrbitalCamera orbital_manipulator;

	float viewport_width = 1600.0f;
	float viewport_height = 800.0f;

	float animation_time = 0.0f;
	bool paused = false;

	static constexpr int PetalCount = 5;    // arcs chasing each other around a rotating ring
	static constexpr int RibbonLinks = 6;   // Catmull-Rom spline of cubics, waving through Z
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

	// One renderer's worth of scene. The four example groups are added once in BuildScene() and kept
	// for the lifetime of the app, so animation reposes them in place through these handles every
	// frame instead. x_offset slides the whole copy sideways so the copies can be compared;
	// BezierCurve itself is renderer-agnostic, so the same Scene type serves both. In the
	// BezierRenderer a handle stays valid when its curve moves to another backend.
	//
	// Every BezierCurve here OWNS its curve (see bezier_common.h): destroying the handle removes the
	// curve from its renderer. That is how the Test screen removes curves - it simply erases entries
	// from test_curves - and why a Scene is move-only.
	//
	// test_curves grows from the Test screen's Add buttons and shrinks from its Remove buttons.
	struct Scene
	{
		float x_offset = 0.0f;
		BezierCurve wave;
		BezierCurve petals[PetalCount];
		// RibbonLinks cubics through RibbonLinks + 1 knots, plus a phantom knot at each end.
		CatmullRomSpline ribbon;
		BezierCurve gallery[GalleryCount];
		std::vector<TestCurve> test_curves;
	};

	Scene curve_scene;
	Scene line_scene;

	// Columns, left to right: curve_renderer / curve_scene, then line_renderer / line_scene.
	static constexpr int ColumnCount = 2;

	// --- screens -------------------------------------------------------------------
	// Two tabs in the control window. Example is the hand-built demo scene; Test spawns random curves
	// into one renderer at a time so the Timings window has something to measure. Both screens share
	// the renderer checkboxes, the style controls, and the same two scenes - the tab only decides
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
	bool draw_curves = true;
	bool draw_lines = true;
	// World-space gap between the two copies when both are visible, centred on the origin - see
	// Update().
	float compare_offset = 7.0f;

	// --- runtime-editable style, applied to every curve in the scene every frame ---
	float style_width = 10.0f;      // half-width, in pixels
	int style_cap_front = 2;        // CurveCap at the P0 end
	int style_cap_back = 0;         // CurveCap at the far end
	bool link_caps = false;         // drive both ends from the front picker
	int style_join = 0;             // CurveJoin
	// The pattern is two numbers, and the BezierRenderer picks the backend from them (KindOf):
	// `patterned` off uploads spacing 0 -> Solid; dash length 0 -> Dots; both set -> Patterned. A dot
	// is dash length 0 with both caps Round; the "Make dots" button sets exactly that.
	bool style_patterned = true;     // off => spacing 0 => solid
	float style_dash_length = 80.0f; // LENGTH of one dash, in pixels (0 = dots)
	float style_spacing = 0.35f;     // world units between pattern centers
	int style_resolution = 120;      // sample points per curve
	float style_min_height = 0.0f;  // colour-by-height band; min >= max blends by curve t instead
	float style_max_height = 0.0f;

	// Merged curves (BezierData::merge_with_previous). The ribbon is the only group built as a chain
	// (CatmullRomSpline::Merged): on, its links 1.. continue link 0 and the six cubics are drawn as one
	// stroke; off, each link is its own stroke with its own caps and its own pattern start. Everything
	// else is always separate.
	bool ribbon_merged = true;

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
	// The timings window. Reads each renderer's public Profiler: one column per BezierRenderer
	// backend, then the line renderer.
	void ProfilerGui();
	void Render();

	// Adds one copy of the scene to `renderer` and fills `scene` with handles to it.
	void BuildScene(CurveRenderer& renderer, Scene& scene, float x_offset);
	// Spawns test_count random curves into ONE renderer and records them in that renderer's scene.
	// Deliberately per renderer rather than broadcast: the scenes are allowed to diverge, which is the
	// only way to give one renderer a heavier load than the other and watch the Timings table split.
	void AddTestCurves(CurveRenderer& renderer, Scene& scene);
	// Drops the `count` most recently added test curves of one scene (all of them if it holds fewer).
	// Erasing the handles is the removal - each one takes its curve out of the renderer as it goes.
	void RemoveTestCurves(Scene& scene, size_t count);
	// Re-poses and re-colours one copy for the current animation_time and x_offset.
	void PoseScene(Scene& scene);
	// Pushes the runtime style controls onto one copy. The gallery keeps its own caps. In the
	// BezierRenderer this is also what moves curves between backends, on its next Draw().
	void ApplyStyle(Scene& scene);

public:
	SDL_AppResult Init();
	SDL_AppResult Iterate(float delta);
	SDL_AppResult Event(const SDL_Event& Event);
	void Quit(SDL_AppResult result);
};
