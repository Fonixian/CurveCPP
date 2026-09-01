#include "common_includes.h"
#ifdef PLATFORM_WINDOWS
#include "RWStructuredBuffer.h"
#include "Infrastructure/BitwiseOperations.h"

using namespace Axodox::Infrastructure;
using namespace winrt;

namespace Axodox::Graphics
{
    RWStructuredBuffer::RWStructuredBuffer(const GraphicsDevice& device, CapacityOrImmutableData source, uint32_t itemSize) :
        GraphicsBuffer(device, source, BufferType::RWStructured, itemSize),
        _itemSize(itemSize)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC description_uav;
        zero_memory(description_uav);

        description_uav.Format = DXGI_FORMAT_UNKNOWN;
        description_uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        description_uav.Buffer.NumElements = Capacity();

        check_hresult(_device->CreateUnorderedAccessView(_buffer.get(), &description_uav, _uav.put()));


        D3D11_SHADER_RESOURCE_VIEW_DESC description_view;
        zero_memory(description_view);

        description_view.Format = DXGI_FORMAT_UNKNOWN;
        description_view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        description_view.Buffer.NumElements = Capacity();

        check_hresult(_device->CreateShaderResourceView(_buffer.get(), &description_view, _view.put()));
    }

    uint32_t RWStructuredBuffer::ItemSize() const
    {
        return _itemSize;
    }

    uint32_t RWStructuredBuffer::Capacity() const
    {
        return _capacity / _itemSize;
    }

    uint32_t RWStructuredBuffer::Size() const
    {
        return _size / _itemSize;
    }

    void RWStructuredBuffer::BindUnordered(uint32_t slot, GraphicsDeviceContext* context)
    {
        if (!context) context = _device.ImmediateContext();

        context->BindUnorderedAccessView(_uav.get(), slot);
    }

    void RWStructuredBuffer::BindOrdered(ShaderStage stage, uint32_t slot, GraphicsDeviceContext* context)
    {
        if (!context) context = _device.ImmediateContext();

        context->BindShaderResourceView(_view.get(), stage, slot);
    }
}
#endif
