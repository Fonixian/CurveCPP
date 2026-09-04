#pragma once
#include "bezier_common.h"

class BezierSolidRenderer : public BezierRendererBase {
public:
	explicit BezierSolidRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
};