#pragma once
#ifdef PLATFORM_WINDOWS
#include "GraphicsBuffer.h"

namespace Axodox::Graphics
{
  class AXODOX_COMMON_API RWStructuredBuffer : public GraphicsBuffer
  {
  public:
    template<typename T>
    RWStructuredBuffer(const GraphicsDevice& device, TypedCapacityOrImmutableData<T> source) :
      RWStructuredBuffer(device, source, sizeof(T))
    { }

    uint32_t ItemSize() const;
    uint32_t Capacity() const;
    uint32_t Size() const;

    void BindUnordered(uint32_t slot = 0u, GraphicsDeviceContext* context = nullptr);
    void BindOrdered(ShaderStage stage, uint32_t slot = 0u, GraphicsDeviceContext* context = nullptr);

  private:
    uint32_t _itemSize;
    winrt::com_ptr<ID3D11UnorderedAccessView> _uav;
    winrt::com_ptr<ID3D11ShaderResourceView> _view;

    RWStructuredBuffer(const GraphicsDevice& device, CapacityOrImmutableData source, uint32_t itemSize);
  };
}
#endif
