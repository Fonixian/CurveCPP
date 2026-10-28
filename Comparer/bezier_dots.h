#pragma once
#include "bezier_common.h"
#include "gpu_buffers.h"
#include "SegmentedScan.h"
#include "ParalellScan.h"

class BezierDotRenderer : public BezierRendererBase {
public:
	explicit BezierDotRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

protected:
	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
	void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocateDotBuffer(const Axodox::Graphics::GraphicsDevice& device);
	void CountDots(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunDotPlacementPass(Axodox::Graphics::GraphicsDeviceContext* context);

	bool need_recount = false;

	struct UploadDotStyle {
		uint32_t width_capcap;
		float    spacing;
	};
	static_assert(sizeof(UploadDotStyle) == 8, "UploadDotStyle must match DotStyle in the shaders");

	std::vector<UploadDotStyle> style_scratch;

	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dot_indices;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dots;

	IndirectDrawArgs draw_args;

	Axodox::Graphics::ComputeShader* dot_ini;
	Axodox::Graphics::ComputeShader* dot_calc;
	Axodox::Graphics::ComputeShader* dot_args;
	SegmentedScan<float> scan;
	ParalellScan offset_scan;
};
