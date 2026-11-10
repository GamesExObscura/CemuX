#pragma once
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"
#include "Cafe/CafeSystem.h"
#include <cmath>

inline bool UseNbaDegamma(Latte::E_GX2SURFFMT format, Latte::E_GX2SURFFMT base)
{
    return format == (base | Latte::E_GX2SURFFMT::FMT_BIT_SRGB) &&
        CafeSystem::GetForegroundTitleId() == 0x000500001010da00ull &&
        CafeSystem::GetForegroundTitleVersion() == 32 &&
        CafeSystem::GetRPXHashUpdated() == 0xef62d66e;
}

inline uint32 AdjustNbaR8CompSel(Latte::E_GX2SURFFMT format, uint32 compSel)
{
    // Preserve Latte's single-channel replication after host sRGB conversion.
    return UseNbaDegamma(format, Latte::E_GX2SURFFMT::R8_UNORM) && compSel < 4 ? 0 : compSel;
}

class TextureDecoder_NbaBC4 : public TextureDecoder_BC4_UNORM_uncompress
{
public:
    static TextureDecoder_NbaBC4* getInstance()
    {
        static TextureDecoder_NbaBC4 instance;
        return &instance;
    }

    void decode(LatteTextureLoaderCtx* ctx, uint8* output) override
    {
        TextureDecoder_BC4_UNORM_uncompress::decode(ctx, output);
        // Neither host API has BC4 sRGB. Expand to linear RG32F so degamma
        // still precedes filtering, without quantizing BC4's interpolated values.
        float* pixels = reinterpret_cast<float*>(output);
        const size_t count = static_cast<size_t>(ctx->width) * ctx->height;
        for (size_t i = 0; i < count; ++i)
        {
            const float red = pixels[i * 2];
            pixels[i * 2] = red <= 0.04045f ? red / 12.92f : std::pow((red + 0.055f) / 1.055f, 2.4f);
        }
    }
};

class TextureDecoder_NbaRG8 : public TextureDecoder, public SingletonClass<TextureDecoder_NbaRG8>
{
public:
    sint32 getBytesPerTexel(LatteTextureLoaderCtx*) override { return 4; }

    void decode(LatteTextureLoaderCtx* ctx, uint8* output) override
    {
        for (sint32 y = 0; y < ctx->height; ++y)
            for (sint32 x = 0; x < ctx->width; ++x, output += 4)
                decodePixelToRGBA(LatteTextureLoader_GetInput(ctx, x, y), output, 0, 0);
    }

    void decodePixelToRGBA(uint8* input, uint8* output, uint8, uint8) override
    {
        output[0] = input[0];
        output[1] = input[1];
        output[2] = 0;
        output[3] = 255;
    }
};

class TextureDecoder_NbaR8 : public TextureDecoder_NbaRG8
{
public:
    static TextureDecoder_NbaR8* getInstance()
    {
        static TextureDecoder_NbaR8 instance;
        return &instance;
    }

    void decodePixelToRGBA(uint8* input, uint8* output, uint8, uint8) override
    {
        output[0] = input[0];
        output[1] = 0;
        output[2] = 0;
        output[3] = 255;
    }
};

class TextureDecoder_NbaRGB565 : public TextureDecoder_NbaRG8
{
public:
    static TextureDecoder_NbaRGB565* getInstance()
    {
        static TextureDecoder_NbaRGB565 instance;
        return &instance;
    }

    void decodePixelToRGBA(uint8* input, uint8* output, uint8, uint8) override
    {
        // Latte stores red in the low five bits. Expand before host sRGB sampling.
        const uint32 packed = uint32(input[0]) | (uint32(input[1]) << 8);
        const uint32 red = packed & 31;
        const uint32 green = (packed >> 5) & 63;
        const uint32 blue = (packed >> 11) & 31;
        output[0] = (red << 3) | (red >> 2);
        output[1] = (green << 2) | (green >> 4);
        output[2] = (blue << 3) | (blue >> 2);
        output[3] = 255;
    }
};
