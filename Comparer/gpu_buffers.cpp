#include "gpu_buffers.h"
#include <algorithm>

using namespace Axodox::Graphics;

// --- GpuCounter --------------------------------------------------------------------------------

GpuCounter::GpuCounter(const GraphicsDevice& device, uint32_t frame_latency) : device(device) {
	auto d3d_device = this->device.get();

	// Structured rather than raw so HLSL still sees a plain RWStructuredBuffer<uint> and the ini
	// shaders keep their existing InterlockedAdd untouched.
	D3D11_BUFFER_DESC counter_desc = {};
	counter_desc.ByteWidth = sizeof(uint32_t);
	counter_desc.Usage = D3D11_USAGE_DEFAULT;
	counter_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	counter_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	counter_desc.StructureByteStride = sizeof(uint32_t);

	const uint32_t zero = 0u;
	D3D11_SUBRESOURCE_DATA counter_data = {};
	counter_data.pSysMem = &zero;

	winrt::check_hresult(d3d_device->CreateBuffer(&counter_desc, &counter_data, counter.put()));

	D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
	uav_desc.Format = DXGI_FORMAT_UNKNOWN;
	uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav_desc.Buffer.NumElements = 1u;
	winrt::check_hresult(d3d_device->CreateUnorderedAccessView(counter.get(), &uav_desc, uav.put()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = DXGI_FORMAT_UNKNOWN;
	srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srv_desc.Buffer.NumElements = 1u;
	winrt::check_hresult(d3d_device->CreateShaderResourceView(counter.get(), &srv_desc, srv.put()));

	// Two slots would already work, but a third means a frame that submits and fetches in the same
	// call never finds the ring full while the GPU is one or two frames behind.
	D3D11_BUFFER_DESC staging_desc = {};
	staging_desc.ByteWidth = sizeof(uint32_t);
	staging_desc.Usage = D3D11_USAGE_STAGING;
	staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	slots.resize(std::max(frame_latency, 2u));
	for (auto& slot : slots)
		winrt::check_hresult(d3d_device->CreateBuffer(&staging_desc, nullptr, slot.staging.put()));
}

void GpuCounter::Reset(GraphicsDeviceContext* context) {
	const uint32_t zero = 0u;
	(*context)->UpdateSubresource(counter.get(), 0, nullptr, &zero, 0, 0);
}

void GpuCounter::BindUnordered(uint32_t slot, GraphicsDeviceContext* context) {
	context->BindUnorderedAccessView(uav.get(), slot);
}

void GpuCounter::BindOrdered(ShaderStage stage, uint32_t slot, GraphicsDeviceContext* context) {
	context->BindShaderResourceView(srv.get(), stage, slot);
}

void GpuCounter::Submit(GraphicsDeviceContext* context) {
	if (pending_count == slots.size()) return;

	// CopySubresourceRegion rather than CopyResource: the source is structured and the staging copy is
	// not, and a region copy is defined as a plain byte copy between buffers whatever their misc flags.
	const D3D11_BOX box = { 0u, 0u, 0u, static_cast<UINT>(sizeof(uint32_t)), 1u, 1u };
	(*context)->CopySubresourceRegion(slots[write_slot].staging.get(), 0, 0, 0, 0, counter.get(), 0, &box);

	slots[write_slot].pending = true;
	write_slot = (write_slot + 1u) % slots.size();
	++pending_count;
}

void GpuCounter::Fetch(GraphicsDeviceContext* context) {
	while (pending_count > 0u) {
		auto& slot = slots[read_slot];

		D3D11_MAPPED_SUBRESOURCE mapped = {};
		const HRESULT result = (*context)->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);

		// The whole point of this class: the copy has not landed yet, so leave it and come back next
		// frame instead of waiting for the GPU.
		if (result == DXGI_ERROR_WAS_STILL_DRAWING) return;
		winrt::check_hresult(result);

		last_value = *static_cast<const uint32_t*>(mapped.pData);
		has_value = true;

		(*context)->Unmap(slot.staging.get(), 0);

		slot.pending = false;
		read_slot = (read_slot + 1u) % slots.size();
		--pending_count;
	}
}

// --- IndirectDrawArgs --------------------------------------------------------------------------

IndirectDrawArgs::IndirectDrawArgs(const GraphicsDevice& device, uint32_t vertices_per_instance) : device(device) {
	auto d3d_device = this->device.get();

	// ALLOW_RAW_VIEWS is what makes a UAV possible at all here - DRAWINDIRECT_ARGS and
	// BUFFER_STRUCTURED are mutually exclusive, so the shader side has to be a RWByteAddressBuffer.
	D3D11_BUFFER_DESC args_desc = {};
	args_desc.ByteWidth = 4u * sizeof(uint32_t);
	args_desc.Usage = D3D11_USAGE_DEFAULT;
	args_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	args_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

	// Seeded so a draw issued before the shader has ever run draws nothing rather than garbage.
	const uint32_t initial[4] = { vertices_per_instance, 0u, 0u, 0u };
	D3D11_SUBRESOURCE_DATA args_data = {};
	args_data.pSysMem = initial;

	winrt::check_hresult(d3d_device->CreateBuffer(&args_desc, &args_data, args.put()));

	D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
	uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
	uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav_desc.Buffer.NumElements = 4u;
	uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;

	winrt::check_hresult(d3d_device->CreateUnorderedAccessView(args.get(), &uav_desc, uav.put()));
}

void IndirectDrawArgs::BindUnordered(uint32_t slot, GraphicsDeviceContext* context) {
	context->BindUnorderedAccessView(uav.get(), slot);
}
