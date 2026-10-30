#pragma once
#include "BezierRendererBase.h"
#include "BezierCurve.h"

// Solid strokes (width, caps, joins). No compute passes: SolidVert evaluates the curve itself.
class BezierSolidRenderer : public BezierRendererBase
{
public:
  explicit BezierSolidRenderer(const Axodox::Graphics::GraphicsDevice& device);

  void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
  void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
  void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
};
