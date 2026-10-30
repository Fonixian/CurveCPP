#pragma once
#include "bezier_common.h"
#include "SegmentedScan.h"
#include "ParalellScan.h"

// Dashes: spacing (world arc length between dash centres) and dash_length (pixels). spacing <= 0
// draws nothing. The point pass measures world + screen arc length per sample (one float2 buffer,
// segmented-scanned per chain), pattern_ini counts the centres per curve, offset_scan turns the
// counts into each curve's slice, and pattern_calc places each centre in screen arc length. The
// pattern runs on across the curves of a merged chain. The pattern buffer is sized from the CPU
// bound (PatternBound), so nothing waits on the GPU.
class BezierPatternRenderer : public BezierRendererBase {
public:
	explicit BezierPatternRenderer(const Axodox::Graphics::GraphicsDevice& device);

	void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

	uint32_t PatternCapacity() const override { return patterns_allocated; }

protected:
	void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
	void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
	void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
	// Also updates the pattern bound and need_recount.
	void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;

private:
	void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
	void AllocatePatternBuffer(const Axodox::Graphics::GraphicsDevice& device);
	void CountPatternCenters(Axodox::Graphics::GraphicsDeviceContext* context);
	void RunPatternPass(Axodox::Graphics::GraphicsDeviceContext* context);

	// The count only depends on world arc length and spacing, so it is redone only when those change.
	bool need_recount = false;

	// PatternStyle in curve_common.hlsli.
	struct UploadPatternStyle {
		uint32_t width_capcapjoin;
		float    spacing;
		float    dash_length;
	};
	static_assert(sizeof(UploadPatternStyle) == 12, "UploadPatternStyle must match PatternStyle in the shaders");

	std::vector<UploadPatternStyle> style_scratch;

	uint32_t patterns_allocated = 0;

	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances;       // per sample: (world, screen px) arc length
	// Per curve: centre count, scanned in place into offsets; the total sits one past the last curve.
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> pattern_offsets;
	std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> patterns;        // screen arc length per centre

	Axodox::Graphics::ComputeShader* pattern_ini;
	Axodox::Graphics::ComputeShader* pattern_calc;
	SegmentedScan<DirectX::XMFLOAT2> distance_scan;
	ParalellScan offset_scan;
};
