#pragma once
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"
#include <cstring>

class TextureDecoder_ReversedRGB10A2 : public TextureDecoder, public SingletonClass<TextureDecoder_ReversedRGB10A2>
{
public:
    sint32 getBytesPerTexel(LatteTextureLoaderCtx*) override { return 4; }

    static uint32 Pack(uint32 value)
    {
        // GX2 memory: R[31:22], G[21:12], B[11:2], A[1:0].
        // Vulkan A2B10G10R10: A[31:30], B[29:20], G[19:10], R[9:0].
        return (value >> 22) | (((value >> 12) & 1023u) << 10) |
            (((value >> 2) & 1023u) << 20) | ((value & 3u) << 30);
    }

    void decode(LatteTextureLoaderCtx* ctx, uint8* output) override
    {
        for (sint32 y = 0; y < ctx->height; ++y)
        {
            for (sint32 x = 0; x < ctx->width; ++x)
            {
                uint32 value;
                std::memcpy(&value, LatteTextureLoader_GetInput(ctx, x, y), sizeof(value));
                value = Pack(value);
                std::memcpy(output, &value, sizeof(value));
                output += sizeof(value);
            }
        }
    }

    void decodePixelToRGBA(uint8* block, uint8* output, uint8, uint8) override
    {
        uint32 value;
        std::memcpy(&value, block, sizeof(value));
        output[0] = static_cast<uint8>((value >> 24) & 255u);
        output[1] = static_cast<uint8>((value >> 14) & 255u);
        output[2] = static_cast<uint8>((value >> 4) & 255u);
        output[3] = static_cast<uint8>((value & 3u) * 85u);
    }
};
