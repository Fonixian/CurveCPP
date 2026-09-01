#pragma once
#ifdef PLATFORM_WINDOWS
#include "Graphics/Devices/GraphicsResource.h"

namespace Axodox::Graphics
{
    typedef std::variant<uint32_t, std::span<const uint8_t>> CapacityOrImmutableData;

    template<typename T>
    struct TypedCapacityOrImmutableData : public CapacityOrImmutableData
    {
        static_assert(std::is_trivially_copyable_v<T>);

        typedef T ItemType;

        TypedCapacityOrImmutableData() = default;

        TypedCapacityOrImmutableData(uint32_t capacity) :
            CapacityOrImmutableData(uint32_t(capacity * sizeof(T)))
        {
        }

        TypedCapacityOrImmutableData(std::span<const T> immutableData) :
            CapacityOrImmutableData(std::span<const uint8_t>{ reinterpret_cast<const uint8_t*>(immutableData.data()), immutableData.size() * sizeof(T) })
        {
        }
    };

    enum class BufferType
    {
        Index,
        Vertex,
        Constant,
        Structured,
        RWStructured
    };

    class AXODOX_COMMON_API GraphicsBuffer : public GraphicsResource
    {
    public:
        GraphicsBuffer(const GraphicsDevice& device, CapacityOrImmutableData source, BufferType type, uint32_t itemSize = 0u);

        void Upload(std::span<const uint8_t> buffer, GraphicsDeviceContext* context = nullptr);

        template<typename T>
        void Upload(std::span<const T> items, GraphicsDeviceContext* context = nullptr)
        {
            Upload(std::span<const uint8_t>{ reinterpret_cast<const uint8_t*>(items.data()), items.size() * sizeof(T) }, context);
        }

        //Copies the buffer's current contents back to the CPU, through a temporary staging resource.
        std::vector<uint8_t> Download(GraphicsDeviceContext* context = nullptr);

        template<typename T>
        std::vector<T> Download(GraphicsDeviceContext* context = nullptr)
        {
            static_assert(std::is_trivially_copyable_v<T>);

            auto bytes = Download(context);

            std::vector<T> result(bytes.size() / sizeof(T));
            memcpy(result.data(), bytes.data(), result.size() * sizeof(T));
            return result;
        }

    protected:
        winrt::com_ptr<ID3D11Buffer> _buffer;
        uint32_t _capacity;
        uint32_t _size;
    };
}
#endif PLATFORM_WINDOWS