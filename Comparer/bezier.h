#pragma once
#include "bezier_common.h"
#include "bezier_solid.h"
#include "bezier_pattern.h"
#include "bezier_dots.h"

const char* CurveKindName(CurveKind kind);

// One renderer for every curve: owns a solid, a patterned and a dot renderer and sends each curve to
// the backend its KindOf() picks. Add() returns the backend's own handle. When a setter changes a
// curve's kind, the curve is marked and the next Draw() moves it (and its whole chain, routed by the
// chain's first curve) to the end of the right backend; the handle is re-pointed and stays valid. A
// merged Add() goes to the backend of the previous Add() if that curve is still last there. Empty
// backends are not drawn.
class BezierRenderer : public CurveRenderer {
public:
	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

	BezierRenderer(const BezierRenderer&) = delete;
	BezierRenderer& operator=(const BezierRenderer&) = delete;

	[[nodiscard]] BezierCurve Add(const BezierData& curve) override;
	void Remove(BezierCurve& curve);

	size_t Count() const override;
	// As of the last Route().
	size_t Count(CurveKind kind) const { return Backend(kind).Count(); }

	void SetViewport(float width, float height) override;
	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	// Checks the chains of the marked curves (see BezierRendererBase::route_marks) and moves the ones
	// whose kind no longer matches their backend. Draw() calls it first.
	void Route();

	// For profiling; add curves through the BezierRenderer.
	BezierRendererBase& Backend(CurveKind kind);
	const BezierRendererBase& Backend(CurveKind kind) const;

private:
	BezierSolidRenderer solid;
	BezierPatternRenderer patterned;
	BezierDotRenderer dots;

	// Lets a merged Add() check that the previous Add() is still the last curve of its backend.
	// Route() clears it when it moves curves into that backend.
	BezierRendererBase* last_added = nullptr;
	size_t last_added_slots = 0;

	bool ContinuesLastAdd() const;

	// Whether curve i continues the chain of the slot before it. A removed slot ends a chain, as the
	// next compaction will make it.
	static bool ContinuesChain(const BezierRendererBase& backend, size_t i);
	// Moves the chain [first, end) out of `source` if its first curve's kind is not `home`.
	void RouteChain(BezierRendererBase& source, size_t home, size_t first, size_t end);
};
