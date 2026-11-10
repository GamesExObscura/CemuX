#pragma once
#include <atomic>
#include <cstdint>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace NbaPackedMovie
{
constexpr uint32_t Native = 0x02fc7578;
constexpr uint32_t Call = 0x02fc5674;
constexpr uint32_t Width = 1024, Height = 576, RowBytes = Width * 2;
constexpr uint32_t Bytes = RowBytes * Height;

enum class Proof { Unchecked, Checking, Verified, Rejected };
inline std::atomic<Proof> proof{Proof::Unchecked};

constexpr bool IsLayout(uint32_t width, uint32_t height, uint32_t pitch,
    int32_t outputPitch, uint32_t gqr)
{
    // GQR2 loads/stores unsigned bytes without scaling: merging is byte interleaving.
    return width == Width && height == Height && pitch >= Width && pitch <= Width + 256 && pitch % 16 == 0 &&
        outputPitch == -static_cast<int32_t>(RowBytes) && gqr == 0x00040004;
}

struct Range { uint32_t address, bytes; };
constexpr bool Valid(Range r)
{
    return r.address && r.bytes && uint64_t(r.address) + r.bytes <= 0x100000000ull;
}
constexpr bool Overlap(Range a, Range b)
{
    return uint64_t(a.address) < uint64_t(b.address) + b.bytes &&
        uint64_t(b.address) < uint64_t(a.address) + a.bytes;
}

inline void Pack(const uint8_t* y, const uint8_t* u, const uint8_t* v, uint32_t sourcePitch,
    uint8_t* firstOutputRow, int32_t outputPitch)
{
    for (uint32_t row = 0; row < Height; ++row)
    {
        const auto* luma = y + row * sourcePitch;
        const auto* cb = u + (row / 2) * sourcePitch;
        const auto* cr = v + (row / 2) * sourcePitch;
        auto* out = firstOutputRow + static_cast<int64_t>(row) * outputPitch;
#if (defined(_M_X64) || defined(__x86_64__)) && !defined(CEMU_NBA_PACK_SCALAR_TEST)
        for (uint32_t x = 0; x < Width; x += 32)
        {
            const auto y0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + x));
            const auto y1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(luma + x + 16));
            const auto u0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cb + x / 2));
            const auto v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(cr + x / 2));
            const auto uv0 = _mm_unpacklo_epi8(u0, v0);
            const auto uv1 = _mm_unpackhi_epi8(u0, v0);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x * 2), _mm_unpacklo_epi8(y0, uv0));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x * 2 + 16), _mm_unpackhi_epi8(y0, uv0));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x * 2 + 32), _mm_unpacklo_epi8(y1, uv1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x * 2 + 48), _mm_unpackhi_epi8(y1, uv1));
        }
#else
        for (uint32_t x = 0; x < Width; x += 2)
        {
            out[x * 2] = luma[x];
            out[x * 2 + 1] = cb[x / 2];
            out[x * 2 + 2] = luma[x + 1];
            out[x * 2 + 3] = cr[x / 2];
        }
#endif
    }
}
}
