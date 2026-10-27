#include "curve_switcher.h"
#include <cassert>
#include <utility>

using namespace Axodox::Graphics;
using namespace DirectX;

// --- SwitchedCurve --------------------------------------------------------------------------------

SwitchedCurve::SwitchedCurve(SwitchedCurve&& other) noexcept {
	TakeFrom(other);
}

SwitchedCurve& SwitchedCurve::operator=(SwitchedCurve&& other) noexcept {
	if (this == &other) return *this;

	Remove();
	TakeFrom(other);
	return *this;
}

// Moves everything from `other` into this (empty) handle and re-points every pointer that referred to
// `other`: the switcher's registry, the chain neighbours, and - through BezierCurve's own move - the
// renderer's owner slot.
void SwitchedCurve::TakeFrom(SwitchedCurve& other) noexcept {
	owner = other.owner;
	slot = other.slot;
	kind = other.kind;
	serial = other.serial;
	prev = other.prev;
	next = other.next;
	for (int i = 0; i < 4; ++i) cp[i] = other.cp[i];
	inner = std::move(other.inner);

	if (owner) {
		owner->curves[slot] = this;
		if (prev) prev->next = this;
		if (next) next->prev = this;
	}

	other.owner = nullptr;
	other.prev = nullptr;
	other.next = nullptr;
}

void SwitchedCurve::Remove() {
	if (owner) owner->Detach(*this);
}

CurveKind SwitchedCurve::Kind() const {
	assert(valid());
	const SwitchedCurve* head = this;
	while (head->prev) head = head->prev;
	return ClassifyCurve(head->inner.DashLength(), head->inner.Spacing());
}

void SwitchedCurve::control_points(const XMFLOAT3& P0, const XMFLOAT3& P1) {
	assert(valid());
	cp[0] = P0; cp[1] = LerpFloat3(P0, P1, 1.0f / 3.0f); cp[2] = LerpFloat3(P0, P1, 2.0f / 3.0f); cp[3] = P1;
	inner.control_points(cp[0], cp[1], cp[2], cp[3]);
}

void SwitchedCurve::control_points(const XMFLOAT3& P0, const XMFLOAT3& P1, const XMFLOAT3& P2) {
	assert(valid());
	cp[0] = P0; cp[1] = LerpFloat3(P0, P1, 2.0f / 3.0f); cp[2] = LerpFloat3(P1, P2, 1.0f / 3.0f); cp[3] = P2;
	inner.control_points(cp[0], cp[1], cp[2], cp[3]);
}

void SwitchedCurve::control_points(const XMFLOAT3& P0, const XMFLOAT3& P1, const XMFLOAT3& P2, const XMFLOAT3& P3) {
	assert(valid());
	cp[0] = P0; cp[1] = P1; cp[2] = P2; cp[3] = P3;
	inner.control_points(P0, P1, P2, P3);
}

void SwitchedCurve::KindMayHaveChanged() {
	if (!prev && ClassifyCurve(inner.DashLength(), inner.Spacing()) != kind)
		owner->kinds_dirty = true;
}

void SwitchedCurve::DashLength(float value) {
	assert(valid());
	inner.DashLength(value);
	KindMayHaveChanged();
}

void SwitchedCurve::Spacing(float value) {
	assert(valid());
	inner.Spacing(value);
	KindMayHaveChanged();
}

void SwitchedCurve::Merged(bool value) {
	assert(valid());
	if (Merged() != value) owner->SetMerged(*this, value);
}

// Reads everything back from the inner handle, so a curve keeps exactly what it was drawing with. The
// quantised values (8-bit colour, whole-pixel width) come back unchanged through the next Add: packing
// is idempotent on what unpacking returns.
BezierData SwitchedCurve::Snapshot() const {
	BezierData data;
	data.P0 = cp[0];
	data.P1 = cp[1];
	data.P2 = cp[2];
	data.P3 = cp[3];
	data.C0 = inner.C0();
	data.C1 = inner.C1();
	data.min_height = inner.MinHeight();
	data.max_height = inner.MaxHeight();
	data.width = inner.Width();
	data.cap_front = inner.CapFront();
	data.cap_back = inner.CapBack();
	data.join = inner.Join();
	data.dash_length = inner.DashLength();
	data.spacing = inner.Spacing();
	data.resolution = inner.Resolution();
	data.merge_with_previous = prev != nullptr;
	return data;
}

// --- CurveSwitcher --------------------------------------------------------------------------------

CurveSwitcher::CurveSwitcher(const GraphicsDevice& device)
	: solid_renderer(device), dot_renderer(device), patterned_renderer(device) {
}

CurveSwitcher::~CurveSwitcher() {
	// The renderers detach their own handles (the inner BezierCurves) when they are destroyed right
	// after this; what is left to do is the SwitchedCurve level.
	for (SwitchedCurve* curve : curves) {
		if (!curve) continue;
		curve->owner = nullptr;
		curve->prev = nullptr;
		curve->next = nullptr;
	}
}

BezierRendererBase& CurveSwitcher::Renderer(CurveKind kind) {
	switch (kind) {
	case CurveKind::Solid:  return solid_renderer;
	case CurveKind::Dotted: return dot_renderer;
	default:                return patterned_renderer;
	}
}

const BezierRendererBase& CurveSwitcher::Renderer(CurveKind kind) const {
	switch (kind) {
	case CurveKind::Solid:  return solid_renderer;
	case CurveKind::Dotted: return dot_renderer;
	default:                return patterned_renderer;
	}
}

size_t CurveSwitcher::Count(CurveKind kind) const {
	return Renderer(kind).Count();
}

void CurveSwitcher::SetViewport(float width, float height) {
	solid_renderer.SetViewport(width, height);
	dot_renderer.SetViewport(width, height);
	patterned_renderer.SetViewport(width, height);
}

SwitchedCurve CurveSwitcher::Add(const BezierData& curve) {
	// Like the renderers, a merge with nothing before it (the first curve) is ignored.
	SwitchedCurve* before = curve.merge_with_previous ? LastLive() : nullptr;

	CurveKind kind;
	if (before) {
		// The chain's renderer, whatever this curve's own values say. merge_with_previous means "the
		// curve Added directly before me" INSIDE that renderer, so the chain must still be its last
		// curves; if something was Added there since, the chain goes back to the end first.
		kind = before->kind;
		if (!AtTail(*before)) Rehome(*HeadOf(before), kind);
	}
	else {
		kind = ClassifyCurve(curve.dash_length, curve.spacing);
	}

	BezierData data = curve;
	data.merge_with_previous = before != nullptr;

	// Registered from inside the handle's own storage, like BezierRendererBase::Add: if the return is
	// not elided the move constructor re-points the registry and the chain neighbours.
	SwitchedCurve handle;
	handle.owner = this;
	handle.slot = curves.size();
	handle.kind = kind;
	handle.serial = ++tail_serial[size_t(kind)];
	handle.cp[0] = data.P0;
	handle.cp[1] = data.P1;
	handle.cp[2] = data.P2;
	handle.cp[3] = data.P3;
	handle.inner = Renderer(kind).Add(data);

	assert(!before || !before->next);
	handle.prev = before;
	if (before) before->next = &handle;

	curves.push_back(&handle);
	++live_count;
	return handle;
}

void CurveSwitcher::Remove(SwitchedCurve& curve) {
	if (curve.owner == this) curve.Remove();
}

// The inner curve goes through BezierCurve::Remove, which is lazy (the renderer compacts on its next
// UpdateBuffers and splits the chain there); this mirrors that at the switcher level: the registry slot
// is nulled and compacted in Sync(), and the next curve of the chain starts a stroke of its own.
void CurveSwitcher::Detach(SwitchedCurve& curve) {
	assert(curves[curve.slot] == &curve);

	if (curve.prev) curve.prev->next = nullptr;
	if (curve.next) {
		curve.next->prev = nullptr;
		// It was drawn the way its old head dictated; its own values may say something else.
		kinds_dirty = true;
	}

	curve.inner.Remove();
	curves[curve.slot] = nullptr;
	++removed_count;
	--live_count;

	curve.owner = nullptr;
	curve.prev = nullptr;
	curve.next = nullptr;
}

void CurveSwitcher::SetMerged(SwitchedCurve& curve, bool value) {
	if (!value) {
		curve.prev->next = nullptr;
		curve.prev = nullptr;
		curve.inner.Merged(false);
		// `curve` heads a chain of its own now and is judged by its own values.
		kinds_dirty = true;
		return;
	}

	SwitchedCurve* before = PredecessorOf(curve);
	if (!before) return;

	// Chain links only ever join neighbours in the registry, so the curve before this one ends its chain.
	assert(!before->next);
	before->next = &curve;
	curve.prev = before;

	// The joined chain is one stroke in `before`'s renderer. This curve and the ones after it may be
	// anywhere in theirs, so all of it is Added again, in order.
	SwitchedCurve* head = HeadOf(before);
	Rehome(*head, head->kind);
}

SwitchedCurve* CurveSwitcher::LastLive() const {
	for (size_t i = curves.size(); i-- > 0;)
		if (curves[i]) return curves[i];
	return nullptr;
}

SwitchedCurve* CurveSwitcher::PredecessorOf(const SwitchedCurve& curve) const {
	for (size_t i = curve.slot; i-- > 0;)
		if (curves[i]) return curves[i];
	return nullptr;
}

SwitchedCurve* CurveSwitcher::HeadOf(SwitchedCurve* curve) {
	while (curve->prev) curve = curve->prev;
	return curve;
}

bool CurveSwitcher::AtTail(const SwitchedCurve& curve) const {
	return curve.serial == tail_serial[size_t(curve.kind)];
}

void CurveSwitcher::Rehome(SwitchedCurve& head, CurveKind kind) {
	// Everything is read before anything moves: Adding link k+1 must not depend on link k's old home.
	rehome_scratch.clear();
	for (const SwitchedCurve* link = &head; link; link = link->next)
		rehome_scratch.push_back(link->Snapshot());

	BezierRendererBase& target = Renderer(kind);
	size_t index = 0;
	for (SwitchedCurve* link = &head; link; link = link->next, ++index) {
		// Move-assigning the new handle removes the old curve from wherever it was. The links are Added
		// one after another with nothing in between, so they sit side by side in `target` with their
		// merge flags (set by Snapshot from the chain) pointing at the right neighbour.
		link->inner = target.Add(rehome_scratch[index]);
		link->kind = kind;
		link->serial = ++tail_serial[size_t(kind)];
	}
}

void CurveSwitcher::Reclassify() {
	// Only the first curve of a chain is looked at: the rest follow it.
	for (SwitchedCurve* curve : curves) {
		if (!curve || curve->prev) continue;

		const CurveKind wanted = ClassifyCurve(curve->inner.DashLength(), curve->inner.Spacing());
		if (wanted != curve->kind) Rehome(*curve, wanted);
	}
}

void CurveSwitcher::CompactCurves() {
	if (removed_count == 0) return;

	size_t write = 0;
	for (SwitchedCurve* curve : curves) {
		if (!curve) continue;
		curves[write] = curve;
		curve->slot = write;
		++write;
	}
	curves.resize(write);
	removed_count = 0;
}

void CurveSwitcher::Sync() {
	if (kinds_dirty) {
		Reclassify();
		kinds_dirty = false;
	}
	CompactCurves();
}

void CurveSwitcher::Draw(GraphicsDevice& device, const XMMATRIX& view_proj) {
	Sync();

	// Fixed order: the technique decides draw order, not when a curve was Added.
	solid_renderer.Draw(device, view_proj);
	dot_renderer.Draw(device, view_proj);
	patterned_renderer.Draw(device, view_proj);
}
