#pragma once
#include "bezier_common.h"
#include "bezier.h"
#include "bezier_solid.h"
#include "bezier_dots.h"
#include <array>
#include <vector>

// The technique that draws a curve.
enum class CurveKind : uint8_t {
	Solid = 0,      // BezierSolidRenderer
	Dotted = 1,     // BezierDotRenderer
	Patterned = 2   // BezierRenderer
};
constexpr size_t CurveKindCount = 3;

// The rule that picks the renderer, from the two numbers that describe a pattern:
//
//   spacing <= 0                  Solid      there is no pattern to place
//   else dash_length <= 0         Dotted     a dash of no length is a dot
//   else                          Patterned
//
// Spacing is tested FIRST on purpose. BezierData defaults both to 0, and a curve that has not been
// given a pattern must be a solid stroke - with the dash length tested first it would be a dot renderer
// curve with no spacing, which draws nothing. `!(x > 0)` also sends negative and NaN values to the
// "zero" side.
constexpr CurveKind ClassifyCurve(float dash_length, float spacing) {
	if (!(spacing > 0.f)) return CurveKind::Solid;
	if (!(dash_length > 0.f)) return CurveKind::Dotted;
	return CurveKind::Patterned;
}

class CurveSwitcher;

// An OWNING handle to one curve in a CurveSwitcher - move-only, like BezierCurve, and with the same
// accessor and setter names, so code written against BezierCurve reads the same. The curve leaves the
// switcher when the handle is destroyed, Remove()d, or move-assigned over, and a handle may outlive the
// switcher (it just becomes !valid()).
//
// What it adds to BezierCurve: which renderer holds the curve is never the caller's business. Change
// DashLength() or Spacing() (or call Dot()) and the curve is moved to the renderer its new values
// call for, at the start of the next CurveSwitcher::Draw(). Nothing else about the curve changes.
//
// Inside, the curve is an ordinary BezierCurve in one of the three renderers (`inner`), plus a copy of
// its control points - a BezierCurve has no getter for them, and moving a curve means Adding it again.
// Everything else is read back from the inner handle when it moves, so the getters return the same
// quantised values BezierCurve's do (8-bit colour, whole-pixel width).
class SwitchedCurve {
	friend class CurveSwitcher;
public:
	SwitchedCurve() = default;
	~SwitchedCurve() { Remove(); }

	SwitchedCurve(const SwitchedCurve&) = delete;
	SwitchedCurve& operator=(const SwitchedCurve&) = delete;

	SwitchedCurve(SwitchedCurve&& other) noexcept;
	// Removes the curve this handle owned (if any), then takes over `other`'s.
	SwitchedCurve& operator=(SwitchedCurve&& other) noexcept;

	// Removes the curve from the switcher; the handle is invalid afterwards. Safe on an invalid handle.
	// Like BezierCurve::Remove, this SPLITS a merged chain: the curve that followed starts a new stroke.
	void Remove();

	inline bool valid() const { return owner != nullptr; }

	// The technique this curve is drawn with - what its DashLength() and Spacing() (those of the first
	// curve of its chain, for a merged curve) classify to right now. Reflects a change at once, even
	// though the curve only changes renderer at the next Draw().
	CurveKind Kind() const;

	// Lines and quadratics are raised to cubic here, as BezierCurve does.
	void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1);
	void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2);
	void control_points(const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, const DirectX::XMFLOAT3& P2, const DirectX::XMFLOAT3& P3);

	inline DirectX::XMFLOAT3 C0() const { return inner.C0(); }
	inline DirectX::XMFLOAT3 C1() const { return inner.C1(); }
	inline float MinHeight() const { return inner.MinHeight(); }
	inline float MaxHeight() const { return inner.MaxHeight(); }
	inline float Width() const { return inner.Width(); }
	inline CurveCap CapFront() const { return inner.CapFront(); }
	inline CurveCap CapBack() const { return inner.CapBack(); }
	inline CurveJoin Join() const { return inner.Join(); }
	inline float DashLength() const { return inner.DashLength(); }
	inline float Spacing() const { return inner.Spacing(); }
	inline unsigned Resolution() const { return inner.Resolution(); }
	inline bool Merged() const { return prev != nullptr; }

	inline void colors(const DirectX::XMFLOAT3& C0, const DirectX::XMFLOAT3& C1) { inner.colors(C0, C1); }
	inline void HeightRange(float min, float max) { inner.HeightRange(min, max); }
	inline void Width(float value) { inner.Width(value); }
	inline void Cap(CurveCap front, CurveCap back) { inner.Cap(front, back); }
	inline void Join(CurveJoin value) { inner.Join(value); }
	inline void Resolution(unsigned value) { inner.Resolution(value); }

	// These three can change the curve's kind, so they tell the switcher to look again before drawing.
	void DashLength(float value);
	void Spacing(float value);

	// See BezierData::merge_with_previous; the previous curve is the one Added directly before this one
	// to the SWITCHER (not to any one renderer). Merged(true) with no curve before this one does
	// nothing, Merged(false) splits the chain here. Both are layout changes and do nothing when the
	// value already matches.
	//
	// A merged chain is always drawn by ONE renderer - the one its first curve's DashLength() and
	// Spacing() select - so keep those uniform along a chain, as the renderers already assume. A link
	// whose own values disagree with its head's is drawn the way the head dictates.
	void Merged(bool value);

private:
	CurveSwitcher* owner = nullptr;
	size_t slot = 0;                    // index in owner->curves, which is Add order
	CurveKind kind = CurveKind::Solid;  // the renderer `inner` is in
	// Serial of the last Add of `inner` to its renderer: how a chain finds out whether it is still the
	// last thing in that renderer (see CurveSwitcher::AtTail).
	uint64_t serial = 0;
	// The chain this curve is in: the curve before / after it, nullptr at the ends. A chain is a run of
	// curves merged end to end; its first curve has no `prev`.
	SwitchedCurve* prev = nullptr;
	SwitchedCurve* next = nullptr;
	DirectX::XMFLOAT3 cp[4] = {};       // the cubic control points, as stored in the renderer
	BezierCurve inner;

	// Everything needed to Add this curve to a renderer again.
	BezierData Snapshot() const;
	void TakeFrom(SwitchedCurve& other) noexcept;
	// Called after a setter changed DashLength() / Spacing(): raises the switcher's flag if this curve
	// now classifies differently from the renderer it is in. Only the first curve of a chain counts.
	void KindMayHaveChanged();
};

// The solid, dotted and patterned curve renderers behind one interface, so no curve is ever assigned to
// a renderer by hand: a curve with no spacing is drawn solid, one with spacing but no dash length as dots,
// and one with both as a dash pattern (ClassifyCurve). Changing those values later moves the curve; the
// move is invisible to the caller except for draw order - see below. The plain line renderer is not part
// of this.
//
// Add() hands back a SwitchedCurve, which is used exactly like a BezierCurve. Draw() first moves every
// curve whose values now call for another renderer, then draws the three renderers in a fixed order:
// solid, dotted, patterned.
//
// A move is a Remove + Add, so a curve that changes technique lands at the END of the new renderer's
// curves, and curves of different techniques are drawn in renderer order rather than Add order. That
// only matters where curves overlap. A merged chain moves as a whole, in chain order, so it stays
// contiguous - the renderers need that for merge_with_previous to mean anything.
//
// The renderers are public through Solid() / Dotted() / Patterned() for their profilers and capacity
// counters. Do not Add curves to them directly through the base class and expect the switcher to know
// about them: it only moves curves it owns.
class CurveSwitcher {
	friend class SwitchedCurve;
public:
	explicit CurveSwitcher(const Axodox::Graphics::GraphicsDevice& device);
	// Detaches every handle still alive (they become !valid()), so the switcher may be destroyed before
	// or after the handles pointing into it.
	~CurveSwitcher();

	CurveSwitcher(const CurveSwitcher&) = delete;
	CurveSwitcher& operator=(const CurveSwitcher&) = delete;

	// The renderer for the curve is chosen from curve.dash_length and curve.spacing, except for a curve
	// with merge_with_previous, which joins the chain of the curve Added directly before it and is drawn
	// by that chain's renderer. As in BezierCurve, discarding the returned handle removes the curve again.
	[[nodiscard]] SwitchedCurve Add(const BezierData& curve);
	// Same as curve.Remove(). Does nothing for a handle that is invalid or belongs to another switcher.
	void Remove(SwitchedCurve& curve);

	// Live curves, and live curves currently held by one renderer. The per-kind counts follow the
	// renderers, so a curve whose pattern was just changed is counted under its old kind until Draw().
	size_t Count() const { return live_count; }
	size_t Count(CurveKind kind) const;

	void SetViewport(float width, float height);
	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj);

	BezierSolidRenderer& Solid() { return solid_renderer; }
	BezierDotRenderer& Dotted() { return dot_renderer; }
	BezierRenderer& Patterned() { return patterned_renderer; }
	const BezierSolidRenderer& Solid() const { return solid_renderer; }
	const BezierDotRenderer& Dotted() const { return dot_renderer; }
	const BezierRenderer& Patterned() const { return patterned_renderer; }
	BezierRendererBase& Renderer(CurveKind kind);
	const BezierRendererBase& Renderer(CurveKind kind) const;

private:
	BezierSolidRenderer solid_renderer;
	BezierDotRenderer dot_renderer;
	BezierRenderer patterned_renderer;

	// Every live handle in Add order, which is also the order merge_with_previous looks back through.
	// Removal is lazy like the renderers': it nulls the slot and the next Draw() compacts, so removing n
	// curves is one O(n) pass rather than n of them.
	std::vector<SwitchedCurve*> curves;
	size_t removed_count = 0;
	size_t live_count = 0;

	// Raised by anything that can change which renderer a curve belongs in; Draw() then re-reads the
	// first curve of every chain. Untouched frames cost one bool test.
	bool kinds_dirty = false;

	// How many curves have ever been Added to each renderer through the switcher. A curve remembers the
	// value from its own Add, so `serial == tail_serial` means nothing was Added after it.
	std::array<uint64_t, CurveKindCount> tail_serial = {};

	std::vector<BezierData> rehome_scratch;

	// Brings the renderers in line with the curves' values, then compacts `curves`.
	void Sync();
	void Reclassify();
	void CompactCurves();

	// Takes the whole chain starting at `head` out of its renderer(s) and Adds it, in order, to the
	// renderer for `kind`. Used to change a chain's kind, and to put a chain back at the end of its own
	// renderer when something else was Added there after it.
	void Rehome(SwitchedCurve& head, CurveKind kind);
	// Whether nothing has been Added to `curve`'s renderer since `curve` - the condition for a merged
	// curve to be Added right behind it. Conservative: a removal after it also answers false.
	bool AtTail(const SwitchedCurve& curve) const;

	SwitchedCurve* LastLive() const;
	SwitchedCurve* PredecessorOf(const SwitchedCurve& curve) const;
	static SwitchedCurve* HeadOf(SwitchedCurve* curve);

	void Detach(SwitchedCurve& curve);
	void SetMerged(SwitchedCurve& curve, bool value);
};
