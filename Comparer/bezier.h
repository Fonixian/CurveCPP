#pragma once
#include "bezier_common.h"
#include "bezier_solid.h"
#include "bezier_pattern.h"
#include "bezier_dots.h"

// For drawing solid, patterned, dotted curves
class BezierRenderer : public CurveRenderer {
public:
	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);

	BezierRenderer(const BezierRenderer&) = delete;
	BezierRenderer& operator=(const BezierRenderer&) = delete;

	[[nodiscard]] BezierCurve Add(const BezierData& curve) override;
	void Remove(BezierCurve& curve);

	// Count be out of date, when the type of the curve chagnes.
	size_t Count() const override;
	size_t Count(CurveKind kind) const { return Backend(kind).Count(); }

	void SetViewport(float width, float height) override;
	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	// Reassings curves to renderers.
	void Route();
	
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

	// Whether curve i continues the chain of the slot before it. A removed slot ends a chain, as the
	// next compaction will make it.
	static bool ContinuesChain(const BezierRendererBase& backend, size_t i);
	// Moves the chain [first, end) out of `source` if its first curve's kind is not `home`.
	void RouteChain(BezierRendererBase& source, size_t home, size_t first, size_t end);
};
