#pragma once
#include <cstdint>

namespace MarioKart8LobbyChannels
{
    constexpr std::uint32_t AdjustShadowSwizzle(std::uint64_t titleId, std::uint32_t version,
        std::uint32_t rpxHash, std::uint64_t shaderHash, bool pixelShader,
        std::uint32_t textureUnit, std::uint32_t format, std::uint32_t width,
        std::uint32_t height, std::uint32_t depth, std::uint32_t mips,
        std::uint32_t slices, bool texture2D, std::uint32_t word4)
    {
        if (titleId != 0x000500001010ec00ull || version != 1 || rpxHash != 0x9708a1d3 ||
            !pixelShader || format != 0x7 || width != 1280 || height != 720 ||
            depth != 1 || mips != 1 || slices != 1 || !texture2D)
            return word4;

        const bool material =
            ((shaderHash == 0x7f01bb428551e3fbull || shaderHash == 0x87ec2ec3cbae4b5cull) && textureUnit == 4) ||
            (shaderHash == 0x3e1906cd409c9149ull && textureUnit == 3);
        if (!material || (word4 & 0x0fff0000u) != 0x00d10000u)
            return word4;

        // Captured lobby GBAR view reads absent B from an RG8 shadow mask.
        // Route that read to stored G, preserving the computed shadow values.
        // This is title/material-specific compatibility, not a global RG8 rule.
        return (word4 & ~(7u << 19)) | (1u << 19);
    }
}
