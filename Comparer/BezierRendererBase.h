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
  DirtyStyles = 1u << 2,    // Width(), Cap(), Join(), DashLength(), Spacing(), Dot()
  DirtyLayout = 1u << 3,    // sample ranges - after a resize / relayout only
  DirtyAll = DirtyPositions | DirtyColors | DirtyStyles | DirtyLayout
};

class BezierCurve;
class BezierRenderer;

// Shared part of the line, solid, patterned and dot renderers: curve storage, sample layout, camera
// constants and the per-curve upload. Curves are kept on the CPU in their GPU form (one vector per
// buffer), so an upload is a plain copy; each buffer is re-sent only when its CurveDirtyBits part is
// dirty. The matching HLSL structs are in the *Common.hlsli files.
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

  // CPU upper bound on pattern centres / dots (sum of PatternCenterBound), what the pattern buffer
  // is sized from. The control polygon bounds the arc length, so it is always enough.
  uint32_t PatternBound() const
  {
    return pattern_upper_bound;
  }

  // Allocated pattern slots; 0 for renderers without a pattern buffer.
  virtual uint32_t PatternCapacity() const
  {
    return 0u;
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

  // Indices in the shaders: first and last sample of the curve, inclusive. A merged curve's first
  // is the previous curve's last.
  struct UploadSampleRange
  {
    uint32_t first_index;
    uint32_t last_index;
  };

  static_assert(sizeof(UploadSampleRange) == 8, "UploadSampleRange must match Indices in the shaders");

  // Parallel vectors indexed by curve, in Add (= chain) order. Remove() only nulls the owners slot;
  // CompactRemoved() squeezes the vectors on the next UpdateBuffers.
  std::vector<DirectX::XMFLOAT3> cubic_points; // 4 per curve
  std::vector<UploadColorData> color_data;
  std::vector<uint32_t> style_words; // PackWidthCapCapJoin
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

  std::unique_ptr<Axodox::Graphics::StructuredBuffer> bezier_data_map; // owning curve per sample
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
