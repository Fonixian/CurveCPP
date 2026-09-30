#pragma once
#include "bezier_common.h"

// Hardware line primitive only: a D3D11 LINELIST (the GL_LINES equivalent) with no geometry shader, so
// every curve is 1px wide whatever its `width`, uncapped, unjoined and unpatterned. The curve data is
// BezierSplitRendererBase's split upload; colour, including the height band, is the only style it
// reads, so it has no style buffer and style setters (Width, Cap, ...) upload nothing.
class BezierLineRenderer : public BezierSplitRendererBase {
public:
	explicit BezierLineRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice&, uint32_t) override {}
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext*) override {}
};
