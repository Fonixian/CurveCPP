#pragma once
#include "bezier_common.h"
#include "SegmentedScan.h"
#include "ParalellScan.h"

class BezierRenderer : public BezierRendererBase {
public:
	explicit BezierRenderer(const Axodox::Graphics::GraphicsDevice& device);
	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;
protected:
	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
	void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;
private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocatePatternBuffer(const Axodox::Graphics::GraphicsDevice& device);
	void CountPatternCenters(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

	bool need_recount = false;

	struct UploadPatternStyle {
		uint32_t width_capcapjoin;
		float    spacing;
		float    dash_length;
	};
	static_assert(sizeof(UploadPatternStyle) == 12, "UploadPatternStyle must match PatternStyle in the shaders");

	std::vector<UploadPatternStyle> style_scratch;

	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_offsets;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> patterns;

	Axodox::Graphics::ComputeShader* pattern_ini;
	Axodox::Graphics::ComputeShader* pattern_calc;
	SegmentedScan<DirectX::XMFLOAT2> distance_scan;
	ParalellScan offset_scan;
};
