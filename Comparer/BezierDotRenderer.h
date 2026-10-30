#pragma once
#include "BezierRendererBase.h"
#include "BezierCurve.h"
#include "IndirectDrawArgs.h"
#include "SegmentedScan.h"
#include "ParallelScan.h"

// Dots: one screen-aligned instanced quad per dot (the GL_POINTS stand-in), placed by world arc
// length and oriented by the projected tangent. spacing <= 0 draws no dots; dash_length is ignored.
// The two halves of a dot are shaped by cap_front / cap_back. The dot count stays on the GPU and
// reaches the draw through DrawInstancedIndirect.

class BezierDotRenderer : public BezierRendererBase
{
public:
  explicit BezierDotRenderer(const Axodox::Graphics::GraphicsDevice& device);

  void Draw(Axodox::Graphics::GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) override;

  uint32_t PatternCapacity() const override
  {
    return dots_allocated;
  }

protected:
  void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) override;
  void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
  void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) override;
  void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) override;
  // Also updates the pattern bound and need_recount.
  void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts) override;

private:
  void RunPointPass(Axodox::Graphics::GraphicsDeviceContext* context);
  void AllocateDotBuffer(const Axodox::Graphics::GraphicsDevice& device);
  void CountDots(Axodox::Graphics::GraphicsDeviceContext* context);
  void RunDotPlacementPass(Axodox::Graphics::GraphicsDeviceContext* context);

  // The count only depends on world arc length and spacing, so it is redone only when those change.
  bool need_recount = false;

  // DotStyle in DotCommon.hlsli (join byte 0).
  struct UploadDotStyle
  {
    uint32_t width_capcap;
    float spacing;
  };

  static_assert(sizeof(UploadDotStyle) == 8, "UploadDotStyle must match DotStyle in the shaders");

  std::vector<UploadDotStyle> style_scratch;

  uint32_t dots_allocated = 0;

  std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> distances; // world arc length per sample
  // Per curve: dot count, scanned in place into offsets; the total sits one past the last curve.
  std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dot_indices;
  std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> dots; // per dot: t along its curve + curve index

  IndirectDrawArgs draw_args;

  Axodox::Graphics::ComputeShader* dot_ini;
  Axodox::Graphics::ComputeShader* dot_calc;
  Axodox::Graphics::ComputeShader* dot_args;
  SegmentedScan<float> scan;
  ParallelScan offset_scan;
};
