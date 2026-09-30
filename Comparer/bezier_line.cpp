#include "bezier_line.h"

using namespace Axodox::Graphics;
using namespace DirectX;

BezierLineRenderer::BezierLineRenderer(const GraphicsDevice& device)
	: BezierRendererBase(device) {
	curve_draw.vs = Pipeline::getVS(device, "line_vert.cso");
	curve_draw.ps = Pipeline::getPS(device, "line_ps.cso");
	curve_draw.states = std::make_shared<PipelineState>(PipelineState{
		BlendState{ device, BlendType::AlphaBlend },
		DepthStencilState{ device, true, D3D11_COMPARISON_LESS },
		RasterizerState{ device, RasterizerFlags::CullNone },
		{ 1.f, 1.f, 1.f, 1.f }
	});
}

// Control points are stored in their NATIVE degree - 2, 3 or 4 per curve - so the live prefix of
// curve_control_points is sum(power + 1) entries. It is allocated for the worst case, 4 per curve, so
// a degree change never has to reallocate (UploadCurves has no device); only the upload shrinks.
void BezierLineRenderer::AllocateCurveBuffers(const GraphicsDevice& device, uint32_t curves_required) {
	curve_control_points.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<XMFLOAT3>(curves_required * 4u)));
	curve_colors.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<LineColorData>(curves_required)));
	curve_indices.reset(new StructuredBuffer(device, TypedCapacityOrImmutableData<LineIndices>(curves_required)));
}

// Each buffer is rewritten whole when its part is dirty - StructuredBuffer::Upload maps with
// WRITE_DISCARD, which leaves anything not written undefined, so a buffer is either skipped entirely
// or refilled for every live curve. The split is per buffer, not per curve.
void BezierLineRenderer::UploadCurves(GraphicsDeviceContext* context, uint8_t parts) {
	if (curves.empty() || !curve_control_points) return;

	if (parts & DirtyPositions) {
		control_point_scratch.clear();

		// Where each curve's control points land depends on the degrees of every curve before it, so a
		// re-pose that changes any degree moves the control-point ranges and the indices must follow.
		bool degrees_changed = uploaded_powers.size() != curves.size();
		uploaded_powers.resize(curves.size());

		[[maybe_unused]] XMFLOAT3 previous_end = {}; // last control point of the curve before, for the shared-endpoint check
		for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
			const BezierData& bez = curves[curveIndex];
			const int power = bez.power();
			assert(power >= 1 && power <= 3);

			const XMFLOAT3* points[4] = { &bez.P0, &bez.P1, &bez.P2, &bez.P3 };
			for (int k = 0; k <= power; ++k)
				control_point_scratch.push_back(*points[k]);

			assert((IsChainStart(curveIndex) || EndpointsMeet(previous_end, bez.P0)) &&
				"merge_with_previous on a curve that does not start where the previous one ends");
			previous_end = *points[power];

			if (uploaded_powers[curveIndex] != power) {
				uploaded_powers[curveIndex] = static_cast<uint8_t>(power);
				degrees_changed = true;
			}
		}
		curve_control_points->Upload(std::span<const XMFLOAT3>{ control_point_scratch }, context);

		if (degrees_changed) parts |= DirtyLayout;
	}

	if (parts & DirtyColors) {
		color_scratch.resize(curves.size());
		for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
			const BezierData& bez = curves[curveIndex];
			color_scratch[curveIndex] = LineColorData{
				PackFloat3ToR8G8B8A8(bez.C0), PackFloat3ToR8G8B8A8(bez.C1),
				bez.min_height, bez.max_height
			};
		}
		curve_colors->Upload(std::span<const LineColorData>{ color_scratch }, context);
	}

	// Sample ranges: the same first_index / last_index UploadCurveData writes - a merged curve starts ON
	// the previous curve's last sample (see BezierData::merge_with_previous). Control-point ranges: the
	// running offset into curve_control_points, END EXCLUSIVE, so end - begin = power + 1 = the point
	// count Eval() in line_vert.hlsl loops over. Both are rebuilt together; they only move on a layout
	// change or a degree change.
	if (parts & DirtyLayout) {
		index_scratch.resize(curves.size());
		uint32_t current = 0;
		uint32_t control_point = 0;
		for (size_t curveIndex = 0; curveIndex < curves.size(); ++curveIndex) {
			const BezierData& bez = curves[curveIndex];
			const uint32_t first = IsChainStart(curveIndex) ? current : current - 1u;
			const uint32_t last = first + bez.resolution - 1u;
			const uint32_t bez_end = control_point + static_cast<uint32_t>(bez.power()) + 1u;
			index_scratch[curveIndex] = LineIndices{ first, last, control_point, bez_end };
			current = last + 1u;
			control_point = bez_end;
		}
		assert(current == total_points);
		curve_indices->Upload(std::span<const LineIndices>{ index_scratch }, context);
	}
}

void BezierLineRenderer::Draw(GraphicsDevice& device, const DirectX::XMMATRIX& view_proj) {
	auto* context = device.ImmediateContext();
	BeginDraw();

	UpdateBuffers(device, context);

	if (total_points < 2 || !curve_control_points) {
		EndDraw();
		return;
	}

	UploadCameraData(view_proj, context);

	profiler.begin_gpu("draw");
	curve_draw.Bind(context);

	curve_begins->BindOrdered(ShaderStage::Vertex, 1, context);
	curve_control_points->Bind(ShaderStage::Vertex, 3, context);
	bezier_data_map->Bind(ShaderStage::Vertex, 4, context);
	curve_colors->Bind(ShaderStage::Vertex, 5, context);
	curve_indices->Bind(ShaderStage::Vertex, 6, context);

	viewport_data->Bind(ShaderStage::Vertex, 1, context);

	// One line per pair of consecutive samples; line_vert.hlsl discards the ones that bridge two chains.
	context->get()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
	context->get()->Draw(2u * (total_points - 1u), 0);
	profiler.end_gpu("draw");

	ClearDrawBindings(context);
	EndDraw();
}
