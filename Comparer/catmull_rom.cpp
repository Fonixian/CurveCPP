#include "catmull_rom.h"
#include <algorithm>
#include <cassert>
#include <cmath>

using namespace DirectX;

namespace {
	// Floor on a non-uniform knot interval. Two coincident knots would otherwise give 0/0 in the
	// tangent below; with the floor the offending term is 0/eps = 0, i.e. the duplicate knot simply
	// contributes no direction.
	constexpr float MinKnotInterval = 1e-4f;

	XMFLOAT3 Store(FXMVECTOR v) {
		XMFLOAT3 result;
		XMStoreFloat3(&result, v);
		return result;
	}

	float KnotInterval(FXMVECTOR a, FXMVECTOR b, float alpha) {
		const float d = XMVectorGetX(XMVector3Length(b - a));
		return std::max(std::pow(d, alpha), MinKnotInterval);
	}

	XMFLOAT3 Mirror(const XMFLOAT3& end, const XMFLOAT3& neighbour) {
		return { 2.0f * end.x - neighbour.x, 2.0f * end.y - neighbour.y, 2.0f * end.z - neighbour.z };
	}
}

// Uniform: the tangent at k1 is (k2 - k0) / 2 and the Hermite -> Bezier step divides it by 3, hence
// the familiar k1 + (k2 - k0) / 6. Spelled exactly as app.cpp's ribbon used to, so it reproduces it.
//
// Non-uniform (Barry-Goldman, differentiated at the knot): with knot intervals d_i = |k_{i+1} - k_i|^alpha
//
//     m1 = (k1 - k0)/d0 - (k2 - k0)/(d0 + d1) + (k2 - k1)/d1
//     m2 = (k2 - k1)/d1 - (k3 - k1)/(d1 + d2) + (k3 - k2)/d2
//
// are the tangents per unit of knot parameter; the segment spans d1 of it, so the Bezier handles are
// k1 + m1 * d1/3 and k2 - m2 * d1/3. At alpha = 0 every d is 1 and this collapses to the uniform form.
void CatmullRomToBezier(const XMFLOAT3& k0, const XMFLOAT3& k1, const XMFLOAT3& k2, const XMFLOAT3& k3,
	float alpha, XMFLOAT3& p1, XMFLOAT3& p2) {
	const XMVECTOR v0 = XMLoadFloat3(&k0);
	const XMVECTOR v1 = XMLoadFloat3(&k1);
	const XMVECTOR v2 = XMLoadFloat3(&k2);
	const XMVECTOR v3 = XMLoadFloat3(&k3);

	if (alpha == 0.0f) {
		p1 = Store(v1 + (v2 - v0) * (1.0f / 6.0f));
		p2 = Store(v2 - (v3 - v1) * (1.0f / 6.0f));
		return;
	}

	const float d0 = KnotInterval(v0, v1, alpha);
	const float d1 = KnotInterval(v1, v2, alpha);
	const float d2 = KnotInterval(v2, v3, alpha);

	const XMVECTOR m1 = (v1 - v0) / d0 - (v2 - v0) / (d0 + d1) + (v2 - v1) / d1;
	const XMVECTOR m2 = (v2 - v1) / d1 - (v3 - v1) / (d1 + d2) + (v3 - v2) / d2;

	const float handle = d1 / 3.0f;
	p1 = Store(v1 + m1 * handle);
	p2 = Store(v2 - m2 * handle);
}

// --- CatmullRomSpline -----------------------------------------------------------------------------

CatmullRomSpline::CatmullRomSpline(CatmullRomSpline&& other) noexcept
	: renderer(other.renderer), links(std::move(other.links)), style(other.style),
	  options(other.options), knot_colored(other.knot_colored) {
	other.renderer = nullptr;
	other.links.clear();
}

CatmullRomSpline& CatmullRomSpline::operator=(CatmullRomSpline&& other) noexcept {
	if (this == &other) return *this;
	links = std::move(other.links);  // destroys (removes) whatever this spline had
	renderer = other.renderer;
	style = other.style;
	options = other.options;
	knot_colored = other.knot_colored;
	other.renderer = nullptr;
	other.links.clear();
	return *this;
}

size_t CatmullRomSpline::LinksFor(size_t knot_count, SplineEnds ends) {
	const size_t outer = (ends == SplineEnds::Phantom) ? 2u : 0u;
	return (knot_count >= outer + 2u) ? knot_count - outer - 1u : 0u;
}

void CatmullRomSpline::Build(BezierRendererBase& target, std::span<const XMFLOAT3> knots,
	const BezierData& link_style, const CatmullRomOptions& link_options) {
	links.clear();

	renderer = &target;
	style = link_style;
	style.bezier_power = 3;
	style.merge_with_previous = false;
	options = link_options;
	knot_colored = false;

	const size_t count = LinksFor(knots.size(), options.ends);
	assert(count >= 1 && "Mirror needs >= 2 knots, Phantom >= 4");

	AddLinks(count);
	Pose(knots);
}

void CatmullRomSpline::SetKnots(std::span<const XMFLOAT3> knots) {
	assert(renderer && "SetKnots() before Build()");

	const size_t count = LinksFor(knots.size(), options.ends);
	assert(count >= 1 && "Mirror needs >= 2 knots, Phantom >= 4");

	if (count < links.size()) {
		// The renderer splits a chain where a curve was removed, so what is left of it stays merged
		// and whatever followed the old tail does not get glued on.
		links.erase(links.begin() + std::ptrdiff_t(count), links.end());
		if (!knot_colored) ApplyGradient();
	}
	else if (count > links.size()) {
		links.clear();
		AddLinks(count);
	}

	Pose(knots);
}

void CatmullRomSpline::AddLinks(size_t count) {
	links.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		BezierData link = style;
		link.merge_with_previous = options.merged && i > 0;
		link.C0 = LerpFloat3(style.C0, style.C1, float(i) / float(count));
		link.C1 = LerpFloat3(style.C0, style.C1, float(i + 1) / float(count));
		links.push_back(renderer->Add(link));
	}
}

void CatmullRomSpline::Pose(std::span<const XMFLOAT3> knots) {
	// Interpolated knot j, for j in [-1, n]: the outer two are the phantoms, passed in or mirrored.
	const bool phantom = options.ends == SplineEnds::Phantom;
	const ptrdiff_t n = ptrdiff_t(links.size()) + 1;
	auto knot = [&](ptrdiff_t j) -> XMFLOAT3 {
		if (phantom) return knots[size_t(j + 1)];
		if (j < 0) return Mirror(knots[0], knots[1]);
		if (j >= n) return Mirror(knots[size_t(n - 1)], knots[size_t(n - 2)]);
		return knots[size_t(j)];
	};

	for (size_t i = 0; i < links.size(); ++i) {
		const ptrdiff_t j = ptrdiff_t(i);
		const XMFLOAT3 k0 = knot(j - 1);
		const XMFLOAT3 k1 = knot(j);
		const XMFLOAT3 k2 = knot(j + 1);
		const XMFLOAT3 k3 = knot(j + 2);

		XMFLOAT3 p1, p2;
		CatmullRomToBezier(k0, k1, k2, k3, options.alpha, p1, p2);
		// Link i ends on exactly the knot link i + 1 starts on - the same XMFLOAT3, bit for bit - which
		// is the shared-endpoint assumption merged curves are built on.
		links[i].control_points(k1, p1, p2, k2);
	}
}

void CatmullRomSpline::Colors(const XMFLOAT3& C0, const XMFLOAT3& C1) {
	style.C0 = C0;
	style.C1 = C1;
	knot_colored = false;
	ApplyGradient();
}

void CatmullRomSpline::ApplyGradient() {
	const float count = float(links.size());
	for (size_t i = 0; i < links.size(); ++i)
		links[i].colors(
			LerpFloat3(style.C0, style.C1, float(i) / count),
			LerpFloat3(style.C0, style.C1, float(i + 1) / count));
}

void CatmullRomSpline::KnotColors(std::span<const XMFLOAT3> colors) {
	assert(colors.size() == links.size() + 1 && "one colour per interpolated knot");
	knot_colored = true;
	for (size_t i = 0; i < links.size(); ++i)
		links[i].colors(colors[i], colors[i + 1]);
}

void CatmullRomSpline::Width(float value) {
	style.width = value;
	for (auto& link : links) link.Width(value);
}

void CatmullRomSpline::Cap(CurveCap front, CurveCap back) {
	style.cap_front = front;
	style.cap_back = back;
	for (auto& link : links) link.Cap(front, back);
}

void CatmullRomSpline::Join(CurveJoin value) {
	style.join = value;
	for (auto& link : links) link.Join(value);
}

void CatmullRomSpline::DashLength(float value) {
	style.dash_length = value;
	for (auto& link : links) link.DashLength(value);
}

void CatmullRomSpline::Spacing(float value) {
	style.spacing = value;
	for (auto& link : links) link.Spacing(value);
}

void CatmullRomSpline::HeightRange(float min, float max) {
	style.min_height = min;
	style.max_height = max;
	for (auto& link : links) link.HeightRange(min, max);
}

void CatmullRomSpline::Dot() {
	style.cap_front = CurveCap::Round;
	style.cap_back = CurveCap::Round;
	style.dash_length = 0.0f;
	for (auto& link : links) link.Dot();
}

void CatmullRomSpline::Resolution(unsigned value) {
	style.resolution = value;
	for (auto& link : links)
		if (link.Resolution() != value) link.Resolution(value);
}

void CatmullRomSpline::Merged(bool value) {
	options.merged = value;
	for (size_t i = 1; i < links.size(); ++i) links[i].Merged(value);
}
