#pragma once
#include <Include/Axodox.Graphics.h>
#include <d3d11.h>

class IndirectDrawArgs {
public:
	explicit IndirectDrawArgs(const Axodox::Graphics::GraphicsDevice& device);

	IndirectDrawArgs(const IndirectDrawArgs&) = delete;
	IndirectDrawArgs& operator=(const IndirectDrawArgs&) = delete;

	void BindUnordered(uint32_t slot, Axodox::Graphics::GraphicsDeviceContext* context);

	ID3D11Buffer* get() const { return args.get(); }

private:
	Axodox::Graphics::GraphicsDevice device;
	winrt::com_ptr<ID3D11Buffer> args;
	winrt::com_ptr<ID3D11UnorderedAccessView> uav;
};
