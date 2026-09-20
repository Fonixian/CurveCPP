#pragma once
#include <Include/Axodox.Graphics.h>

class ParalellScan {
public:
    static constexpr int32_t ThreadGroupSize = 256;

    ParalellScan(const Axodox::Graphics::GraphicsDevice&, int32_t maxElementCount);

    void Scan(Axodox::Graphics::RWStructuredBuffer& values, int32_t count, Axodox::Graphics::GraphicsDeviceContext* context);

    ParalellScan(const ParalellScan&) = delete;
    ParalellScan& operator=(const ParalellScan&) = delete;
    ParalellScan(ParalellScan&&) = default;
    ParalellScan& operator=(ParalellScan&&) = default;
private:
    struct Level {
        int32_t capacity = 0;
        std::unique_ptr<Axodox::Graphics::RWStructuredBuffer> blockSums;
        std::unique_ptr<Axodox::Graphics::ConstantBuffer> constants;
    };

    Axodox::Graphics::ComputeShader* _localScan;
    Axodox::Graphics::ComputeShader* _addBlockOffsets;
    std::vector<Level> _levels;

    void ScanLevel(Axodox::Graphics::RWStructuredBuffer& values, int32_t count, size_t levelIndex, Axodox::Graphics::GraphicsDeviceContext* context);
};
