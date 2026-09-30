#pragma once
#include "bezier_common.h"

// Solid strokes: width, caps and joins, no pattern. The curve data is BezierSplitRendererBase's split
// upload (control points / colours / indices, each re-sent only when its part changes); on top of it
// this renderer keeps one packed uint of style per curve, re-sent only when a style setter fired.
// solid_vert evaluates every sample it needs from those buffers itself, so there is no point pass and
// no per-sample buffer for it to fill.
class BezierSolidRenderer : public BezierSplitRendererBase {
public:
	explicit BezierSolidRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;

private:
	std::vector<uint32_t> style_scratch;
};
