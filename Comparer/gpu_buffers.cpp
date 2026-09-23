#include "gpu_buffers.h"

using namespace Axodox::Graphics;

IndirectDrawArgs::IndirectDrawArgs(const GraphicsDevice& device) : device(device) {
	auto d3d_device = this->device.get();

	D3D11_BUFFER_DESC args_desc = {};
	args_desc.ByteWidth = 4u * sizeof(uint32_t);
	args_desc.Usage = D3D11_USAGE_DEFAULT;
	args_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	args_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

	const uint32_t initial[4] = { 0u, 0u, 0u, 0u };
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
