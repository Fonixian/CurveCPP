#pragma once
#include "bezier_common.h"

// Hardware line primitive only: a D3D11 LINELIST (the GL_LINES equivalent) with no geometry shader, so
// every curve is 1px wide whatever its `width`, uncapped, unjoined and unpatterned. Colour, including
// the height band, is the only style it reads, and that travels in bezier_data - so this renderer has
// no style buffer at all.
class BezierLineRenderer : public BezierRendererBase {
public:
	explicit BezierLineRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	bool NeedsCalculatedPoints() const override { return false; }

	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice&, uint32_t) override {}
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext*) override {}
};
