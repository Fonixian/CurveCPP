#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include "BezierData.h"

class BezierCurve;

// Implemented by every single renderer and by BezierRenderer, so scene code can drive either.
class CurveRenderer
{
public:
  virtual ~CurveRenderer() = default;

  [[nodiscard]] virtual BezierCurve Add(const BezierData& curve) = 0;
  virtual size_t Count() const = 0;
  virtual void SetViewport(float width, float height) = 0;
  virtual void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) = 0;
};
