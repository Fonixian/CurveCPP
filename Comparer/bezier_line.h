#pragma once
#include "bezier_common.h"

// Hardware line primitive only: a D3D11 LINELIST
class BezierLineRenderer : public BezierRendererBase {
public:
	explicit BezierLineRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	uint8_t LayoutBuffers() const override { return LayoutSegmentMap; }
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice&, uint32_t) override {}
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext*) override {}
};
