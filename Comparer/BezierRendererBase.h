#pragma once
#include <Include/Axodox.Graphics.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <span>
#include <cassert>
#include "Pipeline.h"
#include "Profiler.h"
#include "BezierData.h"
#include "BezierHelpers.h"
#include "CurveRenderer.h"

struct CameraDataBuffer
{
  DirectX::XMFLOAT4X4 VP;
  DirectX::XMFLOAT2 wh;
  uint32_t TotalPointCount;
  uint32_t TotalCurveCount;
};

// Which per-curve buffers need re-uploading. DirtyLayout comes only from AllocateBuffers().
enum CurveDirtyBits : uint8_t
{
  DirtyPositions = 1u << 0, // control_points()
  DirtyColors = 1u << 1,    // colors(), HeightRange()
  DirtyStyles = 1u << 2,    // Width(), Cap(), Join(), DashLength(), Spacing()
  DirtyLayout = 1u << 3,    // sample ranges - after a resize / relayout only
  DirtyAll = DirtyPositions | DirtyColors | DirtyStyles | DirtyLayout
};

// Fastest UpdatePatternBound loop measured so far (all renderers), in ns per curve; infinity when
// nothing has been measured since the last reset.
double PatternBoundMinTimeNs();
void ResetPatternBoundMinTime();

class BezierCurve;
class BezierRenderer;

// Shared part of the line, solid, patterned and dot renderers: curve storage, sample layout, camera
// constants and the per-curve upload. Curves are kept on the CPU in their GPU form (one vector per
// buffer), so an upload is a plain copy; each buffer is re-sent only when its CurveDirtyBits part is
// dirty. The matching HLSL structs are in the *Common.hlsli files.
//
// Shaders evaluate P(t) = [P0 P1 P2 P3] * (s^3, 3s^2t, 3st^2, t^3), s = 1 - t: exact at t = 0 / 1, so
// merged joints match bit-for-bit from both sides.
// Sample ranges are inclusive and back to back: curve i owns [first, first + resolution - 1], t = 0..1.
// One map serves both the point passes and the strip / line draws (bezier_data_map): entry i is the
// owning curve of sample i plus the continuation flags of the segment that STARTS on sample i. The
// draws go per sample, so total_points instances / lines; the one per curve that starts on the curve's
// last sample has no segment behind it and the vertex shader culls it (sample == CurveIndices[c].y).
class BezierRendererBase : public CurveRenderer
{
  friend class BezierCurve;
  friend class BezierRenderer; // moves curves between its backends
public:
  explicit BezierRendererBase(const Axodox::Graphics::GraphicsDevice& device);
  // Detaches the remaining handles (they become !valid()).
  virtual ~BezierRendererBase();

  BezierRendererBase(const BezierRendererBase&) = delete;
  BezierRendererBase& operator=(const BezierRendererBase&) = delete;

  // Metrics: update (cpu), calc, scan, pattern, draw, total (cpu + gpu).
  Profiler profiler;

  // The handle owns the curve; dropping it removes the curve.
  [[nodiscard]] BezierCurve Add(const BezierData& curve) override;
  void Remove(BezierCurve& curve);

  size_t Count() const override
  {
    return owners.size() - removed_count;
  }

  void SetViewport(float width, float height) override;

  // CPU upper bound on pattern centres / dots (floor(control polygon / spacing) + 1 per curve), what
  // the pattern buffer is sized from. The control polygon bounds the arc length, so it is always enough.
  uint32_t PatternBound() const
  {
    return pattern_upper_bound;
  }

  // Allocated pattern slots; 0 for renderers without a pattern buffer.
  uint32_t PatternCapacity() const
  {
    return patterns_allocated;
  }

protected:
  // ColorData in the shaders.
  struct UploadColorData
  {
    uint32_t color_begin;
    uint32_t color_end;
    float min_height;
    float max_height;
  };

  static_assert(sizeof(UploadColorData) == 16, "UploadColorData must match ColorData in the shaders");

  // Indices in the shaders: first and last sample of the curve, inclusive. Back to back, merged or
  // not: a curve's first sample is the one right after the previous curve's last.
  struct UploadSampleRange
  {
    uint32_t first_index;
    uint32_t last_index;
  };

  static_assert(sizeof(UploadSampleRange) == 8, "UploadSampleRange must match Indices in the shaders");

  // Parallel vectors indexed by curve, in Add (= chain) order. Remove() only nulls the owners slot;
  // CompactRemoved() squeezes the vectors on the next UpdateBuffers.
  std::vector<DirectX::XMFLOAT3X4> cubic_points; // 1 per curve: rows x/y/z, columns P0..P3 (row_major float3x4 in HLSL)
  std::vector<UploadColorData> color_data;
  std::vector<uint32_t> style_words; // width << 24 | cap_front << 16 | cap_back << 8 | join (width in whole px, dots mask join)
  std::vector<float> spacings;
  std::vector<float> dash_lengths;
  std::vector<uint32_t> resolutions;
  std::vector<uint8_t> merged;
  std::vector<BezierCurve*> owners; // nullptr = removed
  uint32_t removed_count = 0;

  // Removed-but-not-compacted slots included.
  size_t TotalCurves() const
  {
    return owners.size();
  }

  uint32_t pattern_upper_bound = 0;

  bool need_resize = true;
  bool need_upload = false;
  uint8_t upload_parts = 0; // CurveDirtyBits

  uint32_t total_points = 0;

  uint32_t points_allocated = 0;
  uint32_t curves_allocated = 0;
  uint32_t patterns_allocated = 0; // pattern centre / dot buffer, sized from PatternBound(); stays 0 for line and solid

  // Layout buffers (bezier_data_map, curve_begins): created by the renderer's AllocatePointBuffers
  // through the Allocate* helpers below; null = this renderer does not use it, and AllocateBuffers then
  // neither builds nor uploads it.
  std::unique_ptr<Axodox::Graphics::StructuredBuffer> bezier_data_map; // per sample: curve | SegmentHasPrev | SegmentHasNext (of the segment starting there)
  std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_control_points;
  std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_colors;
  std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_indices;
  std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_styles; // layout is per renderer
  std::unique_ptr<Axodox::Graphics::StructuredBuffer> curve_begins; // one bit per sample, set at chain starts

  Axodox::Graphics::ComputeShader* calc_points = nullptr;

  CameraDataBuffer camera_cb_data{};
  std::unique_ptr<Axodox::Graphics::ConstantBuffer> viewport_data;
  Pipeline curve_draw;

  bool IsChainStart(size_t index) const;

  bool UpdateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);
  void UploadCameraData(const DirectX::XMMATRIX& view_proj, Axodox::Graphics::GraphicsDeviceContext* context);

  void UpdatePatternBound();

  // Every Draw() calls BeginDraw() first and EndDraw() on every exit path.
  void BeginDraw();
  void EndDraw();

  bool HasCurveData() const
  {
    return curve_control_points != nullptr;
  }

  void BindCurveData(Axodox::Graphics::GraphicsDeviceContext* context,
    uint32_t control_point_slot, uint32_t color_slot, uint32_t index_slot);

  // Re-uploads the buffers whose part is dirty, each one whole (WRITE_DISCARD). Overrides call this
  // first.
  virtual void UploadCurves(Axodox::Graphics::GraphicsDeviceContext* context, uint8_t parts);

  // Create one layout buffer each; call them from AllocatePointBuffers for the buffers the renderer reads.
  void AllocateSampleMap(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required);   // bezier_data_map
  void AllocateChainBegins(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required); // curve_begins

  // Also creates the layout buffers the renderer reads (it runs before AllocateBuffers builds them).
  virtual void AllocatePointBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t points_required) {}

  virtual void AllocateCurveBuffers(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) {}

  virtual void AllocateStyleBuffer(const Axodox::Graphics::GraphicsDevice& device, uint32_t curves_required) = 0;
  virtual void UploadStyles(Axodox::Graphics::GraphicsDeviceContext* context) = 0;

private:
  void CompactRemoved();
  void AllocateBuffers(const Axodox::Graphics::GraphicsDevice& device, Axodox::Graphics::GraphicsDeviceContext* context);

  // Appends curve `index` to `target` and re-points its handle; the slot here is freed like Remove().
  // `chain_start` clears the merge flag so a moved chain does not join target's last curve.
  void MoveCurve(size_t index, BezierRendererBase& target, bool chain_start);

  // Curves whose chain may belong in another BezierRenderer backend now (kind changed, chain split or
  // joined), for BezierRenderer::Route(). Valid until the next compaction; UpdateBuffers clears them.
  // More marks than curves switch to route_all (check every chain).
  std::vector<uint32_t> route_marks;
  bool route_all = false;
  void MarkForRouting(size_t index);

  std::vector<UploadSampleRange> index_scratch;
};
