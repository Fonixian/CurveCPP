#pragma once
#include "bezier_common.h"
#include <span>
#include <vector>

// How the tangents at the two ends of a spline are found. Catmull-Rom builds every tangent from the
// knots either side of it, so the first and last interpolated knot each need one neighbour that is not
// on the curve.
enum class SplineEnds : uint8_t {
	// The caller passes those neighbours: one extra knot at each end that only shapes the end
	// tangent and is never passed through. N + 2 knots -> N - 1 links.
	Phantom,
	// Every knot is passed through. The missing neighbour is the second knot mirrored through the
	// first (2*K0 - K1), and likewise at the back, so each end leaves heading straight at its
	// neighbouring knot. N knots -> N - 1 links.
	Mirror
};

struct CatmullRomOptions {
	SplineEnds ends = SplineEnds::Mirror;
	// Knot parameterisation: 0 uniform, 0.5 centripetal, 1 chordal. Uniform is the classic 1/6-tangent
	// form and is exact to it. With unevenly spaced knots it can overshoot into loops and cusps;
	// centripetal never does, at the price of a pow() per knot interval.
	float alpha = 0.0f;
	// Links 1.. are Added with merge_with_previous, so the whole spline is one stroke: one arc
	// length, one pattern grid, caps only at the two far ends. Off: one stroke per link.
	bool merged = true;
};

// One Catmull-Rom segment k1 -> k2 (neighbours k0, k3) as the two inner control points of the
// equivalent cubic Bezier; the outer ones are k1 and k2 themselves. Free so it can be tested alone.
void CatmullRomToBezier(const DirectX::XMFLOAT3& k0, const DirectX::XMFLOAT3& k1,
	const DirectX::XMFLOAT3& k2, const DirectX::XMFLOAT3& k3, float alpha,
	DirectX::XMFLOAT3& p1, DirectX::XMFLOAT3& p2);

// A Catmull-Rom spline through a list of knots, drawn as a chain of cubic Bezier links in ONE
// renderer. It owns its links the way a BezierCurve owns its curve: destroying the spline removes them.
//
// Every link shares one style (the BezierData handed to Build(), minus its control points) and the
// setters below push a change to all of them - they are named like BezierCurve's so the same generic
// code can drive either. Link(i) is there for anything per link.
//
// The renderer has to outlive the spline for SetKnots() to be able to add links; the links themselves
// cope with the renderer going first (they just go invalid), same as any BezierCurve.
class CatmullRomSpline {
public:
	CatmullRomSpline() = default;
	CatmullRomSpline(const CatmullRomSpline&) = delete;
	CatmullRomSpline& operator=(const CatmullRomSpline&) = delete;
	CatmullRomSpline(CatmullRomSpline&& other) noexcept;
	CatmullRomSpline& operator=(CatmullRomSpline&& other) noexcept;

	// How many links `knot_count` knots make under `ends`; 0 when that is too few for even one.
	static size_t LinksFor(size_t knot_count, SplineEnds ends);

	// Adds the links to `renderer` (removing any this spline already had) and poses them. `style`
	// supplies everything but the geometry: width, caps, join, pattern, resolution (PER LINK), height
	// band, and C0 -> C1 as a gradient over the whole spline rather than per link. Its control points,
	// bezier_power and merge_with_previous are ignored.
	void Build(BezierRendererBase& renderer, std::span<const DirectX::XMFLOAT3> knots,
		const BezierData& style, const CatmullRomOptions& options = {});

	// Re-poses the links through new knots, same `ends` convention as Build(). Cheap when the link
	// count is unchanged - control points only. A different count is a layout change:
	//   - fewer links: the tail is removed, the rest stay where they are.
	//   - more links:  ALL links are removed and re-Added. A chain has to be contiguous in the renderer
	//                  (merge_with_previous means "the curve Added directly before me"), and anything
	//                  Added after this spline would otherwise sit between the old tail and the new one.
	//                  The spline therefore moves to the end of the renderer's draw order.
	// After a count change a gradient from Colors() is re-spread over the new links; KnotColors() has
	// to be called again.
	void SetKnots(std::span<const DirectX::XMFLOAT3> knots);

	// C0 at the first knot to C1 at the last, spread evenly by link.
	void Colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1);
	// One colour per INTERPOLATED knot, i.e. LinkCount() + 1 of them (phantom knots get none).
	void KnotColors(std::span<const DirectX::XMFLOAT3> colors);

	void Width(float value);
	void Cap(CurveCap front, CurveCap back);
	void Join(CurveJoin value);
	void DashLength(float value);
	void Spacing(float value);
	void HeightRange(float min, float max);
	void Dot();
	// Per link. Only links whose resolution actually differs get the (layout-changing) setter.
	void Resolution(unsigned value);
	unsigned Resolution() const { return style.resolution; }
	// A layout change on links 1.. only when the value actually changes - safe to call every frame.
	void Merged(bool value);
	bool Merged() const { return options.merged; }
	// Takes effect on the next SetKnots().
	void Alpha(float value) { options.alpha = value; }
	float Alpha() const { return options.alpha; }

	SplineEnds Ends() const { return options.ends; }
	const BezierData& Style() const { return style; }

	size_t LinkCount() const { return links.size(); }
	BezierCurve& Link(size_t index) { return links[index]; }
	const BezierCurve& Link(size_t index) const { return links[index]; }
	std::span<BezierCurve> Links() { return links; }

	bool valid() const { return !links.empty() && links.front().valid(); }

private:
	BezierRendererBase* renderer = nullptr;
	std::vector<BezierCurve> links;  // in chain order; reallocation is fine, handles are movable
	BezierData style;                // the shared style; also what newly added links start from
	CatmullRomOptions options;
	bool knot_colored = false;       // last colouring came from KnotColors(), not Colors()

	void AddLinks(size_t count);
	void ApplyGradient();
	void Pose(std::span<const DirectX::XMFLOAT3> knots);
};
