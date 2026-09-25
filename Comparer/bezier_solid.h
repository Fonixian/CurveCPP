#pragma once
#include "bezier_common.h"

class BezierSolidRenderer : public BezierRendererBase {
public:
	explicit BezierSolidRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	// solid_vert evaluates every sample it needs from the curve definitions itself, so there is no
	// point pass and no per-sample buffer for it to fill.
	bool NeedsCalculatedPoints() const override { return false; }

	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
};