#include "BezierCurve.h"
#include <cassert>

BezierCurve::BezierCurve(BezierCurve&& other) noexcept :
  curve_index(other.curve_index),
  renderer(other.renderer)
{
  if (renderer) renderer->owners[curve_index] = this;
  other.renderer = nullptr;
}

BezierCurve& BezierCurve::operator=(BezierCurve&& other) noexcept
{
  if (this == &other) return *this;

  Remove();

  curve_index = other.curve_index;
  renderer = other.renderer;
  if (renderer) renderer->owners[curve_index] = this;
  other.renderer = nullptr;
  return *this;
}

void BezierCurve::Remove()
{
  if (!renderer) return;

  assert(renderer->owners[curve_index] == this);
  renderer->owners[curve_index] = nullptr;
  ++renderer->removed_count;
  renderer->need_resize = true;
  // The next curve may start a chain of its own now.
  if (curve_index + 1 < renderer->owners.size()) renderer->MarkForRouting(curve_index + 1);
  renderer = nullptr;
}
