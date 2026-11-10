#pragma once
#include <cstdint>
#include <span>

namespace RevelationsCommandLookahead
{
constexpr unsigned MaxPrograms = 32;
struct Budget
{
    unsigned words = 65536;
    unsigned draws = 128;
};

// This is a read-only preview, not another command executor. Unknown commands,
// memory writes, waits and state loads terminate the preview before their effects.
template<class Word, class Resolve, class Draw>
bool Scan(std::span<const Word> commands, std::span<std::uint32_t> registers,
    std::uint32_t controlBase, Resolve&& resolve, Draw&& draw, Budget& budget, unsigned depth = 0)
{
    if (depth > 8) return false;
    while (!commands.empty())
    {
        if (!budget.words) return false;
        --budget.words;
        const std::uint32_t header = commands.front();
        commands = commands.subspan(1);
        if ((header >> 30) == 2) continue;
        if ((header >> 30) != 3) return false;
        const unsigned count = ((header >> 16) & 0x3fff) + 1;
        if (count > commands.size() || count > budget.words) return false;
        budget.words -= count;
        const auto data = commands.first(count);
        commands = commands.subspan(count);
        const unsigned op = (header >> 8) & 0xff;
        std::uint32_t base = 0;
        switch (op)
        {
        case 0x68: base = 0x2000; break;
        case 0x69: base = 0xa000; break;
        case 0x6a: base = 0xc000; break;
        case 0x6d: base = 0xe000; break;
        case 0x6e: base = 0xf000; break;
        case 0x6f: base = controlBase; break;
        case 0x27: case 0x2d: case 0x2e:
            if (!budget.draws) return false;
            --budget.draws;
            if (!draw(registers)) return false;
            continue;
        case 0x32:
        {
            if (count != 3 || std::uint32_t(data[1]) != 0) return false;
            const auto nested = resolve(std::uint32_t(data[0]), std::uint32_t(data[2]));
            if (nested.size() != std::uint32_t(data[2]) ||
                !Scan(nested, registers, controlBase, resolve, draw, budget, depth + 1)) return false;
            continue;
        }
        case 0x2a: case 0x2f: case 0x43:
            continue; // Index/instance counts and cache invalidation do not change PS source.
        default:
            return false;
        }
        const std::uint64_t first = std::uint64_t(base) + std::uint32_t(data[0]);
        if (count < 2 || first >= registers.size() || count - 1 > registers.size() - first) return false;
        for (unsigned i = 1; i < count; ++i) registers[first + i - 1] = std::uint32_t(data[i]);
    }
    return true;
}
}

struct LatteContextRegister;
void LatteCP_PrepareUpcomingPixelShaders();
bool LatteShader_PrepareCommandPixelShader(LatteContextRegister& context);
void LatteShader_ClearCommandPreparation();
