#pragma once
#include <cstdint>
#include <array>

namespace RevelationsShaderScheduling
{
    constexpr bool Matches(std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
    {
        return title == 0x000500001012cf00ull && version == 16 && rpx == 0xefa75829;
    }
    constexpr bool LateBinaryLookup(bool loading, bool game, bool custom,
        std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
    {
        return !loading && game && !custom && Matches(title, version, rpx);
    }
    constexpr bool ReadyQueue(bool openGL, bool loading, bool supported,
        std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
    {
        return openGL && loading && supported && Matches(title, version, rpx);
    }
    constexpr bool DeferProgramQueries(bool loading, bool game, bool custom, bool runtimeAllowed,
        std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
    {
        return game && !custom && (loading || runtimeAllowed) && Matches(title, version, rpx);
    }
    constexpr bool LinkFirst(bool requested, bool game, bool custom,
        std::uint64_t title, std::uint32_t version, std::uint32_t rpx)
    {
        return requested && game && !custom && Matches(title, version, rpx);
    }
    template<typename Shader>
    class StageBatch
    {
    public:
        bool Add(Shader* shader)
        {
            if (!shader) return false;
            for (unsigned i = 0; i < m_count; ++i)
                if (m_entries[i] == shader) return true;
            if (m_count == m_entries.size()) return false;
            m_entries[m_count++] = shader;
            return true;
        }
        unsigned Count() const { return m_count; }
        template<typename Finish>
        void Flush(Finish finish)
        {
            const unsigned count = m_count;
            m_count = 0;
            for (unsigned i = 0; i < count; ++i) finish(m_entries[i]);
        }
    private:
        std::array<Shader*, 3> m_entries{};
        unsigned m_count{};
    };
    template<typename Ready>
    int SelectReady(int count, Ready ready)
    {
        for (int i = 0; i < count; ++i)
            if (ready(i)) return i;
        return 0; // Preserve the existing blocking fallback when none is ready.
    }
}
