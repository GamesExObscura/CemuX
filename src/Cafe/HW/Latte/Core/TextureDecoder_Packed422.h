#pragma once
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"

template<bool GB_GR>
class TextureDecoder_Packed422 : public TextureDecoder, public SingletonClass<TextureDecoder_Packed422<GB_GR>>
{
public:
    sint32 getBytesPerTexel(LatteTextureLoaderCtx*) override { return 4; }

    static void Unpack(const uint8* even, const uint8* odd, bool oddPixel, uint8* output)
    {
        // The packed names list high bits first: GB_GR is R,G0,B,G1
        // in memory; BG_RG is G0,R,G1,B. R/B are shared by the pair.
        // Conversion to RGB belongs to the guest shader.
        output[0] = even[GB_GR ? 0 : 1];
        output[1] = (oddPixel ? odd : even)[GB_GR ? 1 : 0];
        output[2] = odd[GB_GR ? 0 : 1];
        output[3] = 255;
    }

    void decode(LatteTextureLoaderCtx* ctx, uint8* output) override
    {
        for (sint32 y = 0; y < ctx->height; ++y)
        {
            for (sint32 x = 0; x < ctx->width; x += 2)
            {
                const uint8* even = LatteTextureLoader_GetInput(ctx, x, y);
                // Odd visible widths can have a padded partner. Never cross
                // the allocated row if an unpadded single pixel is supplied.
                const uint8* odd = x + 1 < ctx->pitch ? LatteTextureLoader_GetInput(ctx, x + 1, y) : even;
                Unpack(even, odd, false, output);
                output += 4;
                if (x + 1 < ctx->width)
                {
                    Unpack(even, odd, true, output);
                    output += 4;
                }
            }
        }
    }

    void decodePixelToRGBA(uint8* block, uint8* output, uint8 x, uint8) override
    {
        Unpack(block, block + 2, (x & 1) != 0, output);
    }
};
