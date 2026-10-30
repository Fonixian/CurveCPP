#include "bezier.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

BezierRenderer::BezierRenderer(const GraphicsDevice& device)
	: solid(device), patterned(device), dots(device) {}

BezierRendererBase& BezierRenderer::Backend(CurveKind kind) {
	switch (kind) {
	case CurveKind::Patterned: return patterned;
	case CurveKind::Dotted:    return dots;
	default:                   return solid;
	}
}

const BezierRendererBase& BezierRenderer::Backend(CurveKind kind) const {
	return const_cast<BezierRenderer*>(this)->Backend(kind);
}

size_t BezierRenderer::Count() const {
	return solid.Count() + patterned.Count() + dots.Count();
}

void BezierRenderer::SetViewport(float width, float height) {
	solid.SetViewport(width, height);
	patterned.SetViewport(width, height);
	dots.SetViewport(width, height);
}

BezierCurve BezierRenderer::Add(const BezierData& curve) {
	BezierData data = curve;
	BezierRendererBase* target = nullptr;

	if (data.merge_with_previous && last_added != nullptr && last_added->owners.size() == last_added_slots && last_added->owners.back() != nullptr) {
		target = last_added;
	}
	else {
		target = &Backend(KindOf(data));
		// Nothing to continue; left set it would join whatever that backend holds last.
		data.merge_with_previous = false;
	}

	BezierCurve handle = target->Add(data);
	last_added = target;
	last_added_slots = target->owners.size();
	return handle;
}

void BezierRenderer::Remove(BezierCurve& curve) {
	solid.Remove(curve);
	patterned.Remove(curve);
	dots.Remove(curve);
}

bool BezierRenderer::ContinuesChain(const BezierRendererBase& backend, size_t i) {
	return i > 0 && backend.owners[i] && backend.merged[i] != 0 && backend.owners[i - 1];
}

void BezierRenderer::RouteChain(BezierRendererBase& source, size_t home, size_t first, size_t end) {
	const auto kind = KindOf(source.spacings[first], source.dash_lengths[first]);
	if (static_cast<size_t>(kind) == home) return;

	BezierRendererBase& target = Backend(kind);
	for (size_t i = first; i < end; ++i)
		source.MoveCurve(i, target, i == first);

	if (&target == last_added) last_added = nullptr;
}

void BezierRenderer::Route() {
	for (size_t home = 0; home < CurveKindCount; ++home) {
		BezierRendererBase& source = Backend(CurveKind(home));

		// Moving only empties slots, so `count` and the marked indices stay valid throughout.
		const size_t count = source.TotalCurves();
		auto chain_end = [&](size_t first) {
			size_t end = first + 1;
			while (end < count && ContinuesChain(source, end)) ++end;
			return end;
		};

		if (source.route_all) {
			for (size_t first = 0; first < count;) {
				if (!source.owners[first]) { ++first; continue; }
				const size_t end = chain_end(first);
				RouteChain(source, home, first, end);
				first = end;
			}
		}
		else if (!source.route_marks.empty()) {
			// Sorted, so each chain is visited once however many of its curves are marked.
			std::sort(source.route_marks.begin(), source.route_marks.end());
			size_t routed_up_to = 0;
			for (const uint32_t marked : source.route_marks) {
				if (marked < routed_up_to || marked >= count || !source.owners[marked]) continue;
				size_t first = marked;
				while (ContinuesChain(source, first)) --first;
				const size_t end = chain_end(first);
				RouteChain(source, home, first, end);
				routed_up_to = end;
			}
		}

		source.route_marks.clear();
		source.route_all = false;
	}
}

void BezierRenderer::Draw(GraphicsDevice& device, const XMMATRIX& view_proj) {
	Route();
	if (solid.Count() > 0) solid.Draw(device, view_proj);
	if (patterned.Count() > 0) patterned.Draw(device, view_proj);
	if (dots.Count() > 0) dots.Draw(device, view_proj);
}
