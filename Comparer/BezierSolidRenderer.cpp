#include "BezierSolidRenderer.h"
#include <algorithm>

using namespace Axodox::Graphics;
using namespace DirectX;

BezierSolidRenderer::BezierSolidRenderer(const GraphicsDevice& device) :
  BezierRendererBase(device)
{
  curve_draw.vs = Pipeline::getVS(device, "SolidVert.cso");
  curve_draw.ps = Pipeline::getPS(device, "SolidPs.cso");
  curve_draw.states = std::make_shared<PipelineState>(PipelineState{
    BlendState{ device, BlendType::AlphaBlend },
    DepthStencilState{ device, true, D3D11_COMPARISON_GREATER },
    RasterizerState{ device, RasterizerFlags::CullNone },
    { 1.f, 1.f, 1.f, 1.f }
  });
}

// SolidCurveStyle in SolidCommon.hlsli: the style word as is.
void BezierSolidRenderer::AllocateStyleBuffer(const GraphicsDevice& device, uint32_t curves_required)
{
  curve_styles.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<uint32_t>(curves_required)));
}

void BezierSolidRenderer::UploadStyles(GraphicsDeviceContext* context)
{
  curve_styles->Upload(std::span<const uint32_t>{ style_words }, context);
}

void BezierSolidRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj)
{
  auto* context = device.ImmediateContext();
  BeginDraw();

  UpdateBuffers(device, context);

  if (total_points < 2 || !HasCurveData())
  {
    EndDraw();
    return;
  }

  UploadCameraData(view_proj, context);

  profiler.begin_gpu("draw");
  curve_draw.Bind(context);

  // Same slots as the patterned renderer's CurveVs.
  BindCurveData(context, 3, 5, 7);                        // t3 control points, t5 colours, t7 indices
  bezier_data_map->Bind(ShaderStage::Vertex, 4, context); // t4: curve + continuation per sample
  curve_styles->Bind(ShaderStage::Vertex, 6, context);

  viewport_data->Bind(ShaderStage::Vertex, 1, context);
  viewport_data->Bind(ShaderStage::Pixel, 1, context);

  context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context->get()->DrawInstanced(5, total_points, 0, 0); // one per sample, curve ends culled in the VS
  profiler.end_gpu("draw");

  UnbindShaderResources(context, ShaderStage::Vertex, { 3, 4, 5, 6, 7 });
  EndDraw();
}
