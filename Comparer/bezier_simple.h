#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include <vector>
#include <span>
#include <memory>
#include "SegmentedScan.h"
#include "pipeline.h"

// Matches the CameraData cbuffer in bezier_calc_points.hlsl (b0, compute) and
// bezier_vert.hlsl / bezier_ps.hlsl (b1, draw).
struct CameraDataBuffer
{
	DirectX::XMFLOAT4X4 VP;
	DirectX::XMFLOAT2 wh;
	float padding[2]; // Align to 16-byte boundary
};

// Matches the DispatchInfo cbuffer in pattern_center_ini.hlsl / pattern_center_calc.hlsl (b0).
struct DispatchInfoBuffer
{
	uint32_t TotalCurveCount;
	uint32_t padding[3]; // Align to 16-byte boundary
};

enum PatternStyle : uint32_t
{
	Solid = 0,
	Segmented = 1,
};

// Which of the two pattern implementations the renderer runs. They share the point pass, the
// scans, the pattern counting and the vertex shader; they differ in what a pattern center IS and
// therefore in the last compute pass and the pixel shader.
enum class PatternMode : uint32_t
{
	// pattern_center_calc.hlsl + bezier_ps.hlsl.
	// A center is a screen ARC LENGTH. The pixel shader cuts the stroke in the curve's own
	// arc-length frame, which bends with the curve - right for a dash, wrong for a shape.
	ArcDash = 0,

	// pattern_center_glyph.hlsl + bezier_glyph_ps.hlsl.
	// A center is a screen POSITION plus a TANGENT. The pixel shader transforms itself into that
	// frame and evaluates an SDF there, so the glyph is a rigid stamp: no bending, no fold-over
	// on tight curvature, correct at any glyph size.
	Glyph = 1,
};

enum class GlyphKind : uint32_t
{
	Dot = 0,
	Arrow = 1,  // points along the direction of travel
	Heart = 2,
	Star = 3,
};

struct Bezier
{
	DirectX::XMFLOAT3 P0;
	DirectX::XMFLOAT3 P1;
	DirectX::XMFLOAT3 P2;
	DirectX::XMFLOAT3 P3;
	DirectX::XMFLOAT3 C0; // Color begin
	DirectX::XMFLOAT3 C1; // color end
	float width;          // Half-width (radius) of the stroke, in PIXELS - constant on screen.
	PatternStyle pattern;
	float spacing;        // WORLD-space arc length between pattern centers.
	int resolution;
};

Bezier linear(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1,
	DirectX::XMFLOAT3 C0, DirectX::XMFLOAT3 C1,
	float width, PatternStyle pattern, float spacing, int resolution);

Bezier quadratic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2,
	DirectX::XMFLOAT3 C0, DirectX::XMFLOAT3 C1,
	float width, PatternStyle pattern, float spacing, int resolution);

Bezier cubic(DirectX::XMFLOAT3 P0, DirectX::XMFLOAT3 P1, DirectX::XMFLOAT3 P2, DirectX::XMFLOAT3 P3,
	DirectX::XMFLOAT3 C0, DirectX::XMFLOAT3 C1,
	float width, PatternStyle pattern, float spacing, int resolution);

// Draws cubic bezier curves as screen-space-thick strokes, optionally cut into a dash pattern
// whose centers are pinned to fixed WORLD-space arc lengths along each curve.
//
// The full algorithm, and what runs when:
//
//   [rebuild only, when the curve set changes]
//     - CPU: sample layout (first/last point index per curve), packed point->curve index map,
//       per-curve segmented-scan reset flags, per-curve spacing and style.
//
//   [every frame, because screen-space arc length depends on the camera]
//     1. bezier_calc_points  - evaluate each sample point, its color, and the length of the
//                              segment that follows it, in BOTH world and screen space.
//     2. SegmentedScan x2    - exclusive per-curve prefix sum of those segment lengths, giving
//                              cumulative world and screen arc length at every sample point.
//
//   [rebuild only, right after the first point pass - counts depend on WORLD arc length and
//    world spacing, both of which are camera independent]
//     3. pattern_center_ini  - per curve, dotCount = floor(worldArcLength / spacing) + 1, and an
//                              InterlockedAdd into a global counter to reserve [first, count) in
//                              one shared pattern buffer. The counter is then read back once
//                              (GraphicsBuffer::Download) to size that buffer.
//
//   [every frame]
//     4. pattern_center_calc - for pattern k of a curve, binary search the WORLD prefix sum for
//                              the segment containing k * spacing, then lerp the SCREEN prefix
//                              sum with the same factor. Result: the screen-space arc length of
//                              every pattern center, written to PatternPosition[first + k].
//     5. draw                - one 5-vertex triangle strip instance per point pair (joins
//                              included). The pixel shader reconstructs its own screen arc
//                              length, picks the nearest pattern center out of PatternPosition,
//                              and shades relative to the distance from it.
class BezierRenderer
{
public:
	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Add(const Bezier& curve); // Only stores cubic bezier curves

	void Clear();

	void SetViewport(float width, float height);
	// Rebuilds curve buffers/geometry if needed, then draws the curves.
	void Draw(const Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj);

	// Switches implementation. The pattern buffer's element type changes with it, so this forces
	// a rebuild - cheap, but not something to call per frame.
	void SetPatternMode(PatternMode mode);
	PatternMode GetPatternMode() const { return pattern_mode; }

	// Glyph appearance, used only in PatternMode::Glyph. 'size' is the glyph's half-extent: world
	// units when worldSized (so it foreshortens with the curve), otherwise pixels. Only re-uploads
	// the per-curve style buffer, so this is safe to drive from a slider.
	//
	// Renderer-level rather than per-curve because the mode already is. The values land in the
	// per-curve style buffer regardless, so making them per-Bezier later is a three-line change.
	void SetGlyph(GlyphKind kind, float size, bool worldSized);

	// Dash length as a multiple of the stroke half-width, in pixels. ArcDash mode only.
	static constexpr float DashLengthPerWidth = 4.0f;

protected:
	// Step 0: (re)create every buffer whose size depends on the curve set.
	void AllocateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	// Rebuilds and re-uploads the per-curve style buffer without touching anything else.
	void RefreshStyles(Axodox::Graphics::GraphicsDeviceContext* context);
	// Steps 1-2: point positions/colors + world and screen segment lengths, then both prefix sums.
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	// Step 3: count pattern centers, reserve their ranges, size the pattern buffer.
	void CountPatternCenters(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
	// Step 4: screen-space position of every pattern center.
	void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

	std::vector<Bezier> curves;
	bool need_resize = false;   // True if buffers need to be rebuilt
	bool need_recount = false;  // True if pattern center counts/ranges need to be rebuilt
	bool styles_dirty = false;  // True if only the per-curve style buffer needs re-uploading
	int total = 0;              // total point count across every curve
	uint32_t pattern_total = 0; // total pattern center count across every curve

	PatternMode pattern_mode = PatternMode::ArcDash;
	PatternMode patterns_layout = PatternMode::ArcDash; // what 'patterns' was allocated for
	GlyphKind glyph_kind = GlyphKind::Dot;
	float glyph_size = 0.18f;
	bool glyph_world_sized = true;

	// --- rebuild-time inputs -------------------------------------------------------
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data;     // Curve definitions
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   bezier_data_map; // Packed curve index per point (2 x uint16 per uint32)
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_spacing;   // World-space pattern spacing per curve
	std::unique_ptr<Axodox::Graphics::StructuredBuffer>   curve_styles;    // Width / style / dash length per curve

	// --- per-frame compute results -------------------------------------------------
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> calculated_points;  // Computed point positions and colors
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;          // Cumulative curve distance per point (world)
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances_screen;   // Cumulative curve distance per point (screen)
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> curve_begins;       // Bit-packed flags for segmented scan reset
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_counter;    // Single uint, atomically summed pattern count
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_ranges;     // Per curve: uint2(first pattern, pattern count)
	// ArcDash: one float per pattern  - screen arc length of the center.
	// Glyph:   one float4 per pattern - screen position (xy) and screen tangent (zw).
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> patterns;

	std::unique_ptr<Axodox::Graphics::ConstantBuffer> dispatch_info;

	Axodox::Graphics::ComputeShader* calc_points;
	Axodox::Graphics::ComputeShader* pattern_ini;
	Axodox::Graphics::ComputeShader* pattern_calc;  // ArcDash
	Axodox::Graphics::ComputeShader* pattern_glyph; // Glyph
	SegmentedScan scan;

	// --- curve-body draw -----------------------------------------------------------
	CameraDataBuffer camera_cb_data{};
	std::unique_ptr<Axodox::Graphics::ConstantBuffer> viewport_data; // Camera / viewport parameters
	Pipeline curve_draw;       // ArcDash: bezier_vert + bezier_ps
	Pipeline curve_draw_glyph; // Glyph:   bezier_vert + bezier_glyph_ps
};
