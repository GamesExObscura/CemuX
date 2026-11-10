#pragma once
#include <cstdint>
#include <functional>
#include <string_view>
#include <memory>
#include <vector>

namespace RevelationsPipelinePreparation
{
constexpr unsigned MaxPending = 8;
constexpr bool RetireUnused(bool completed, std::uint32_t submittedFrame, std::uint32_t currentFrame)
{
    return completed && submittedFrame != currentFrame;
}
constexpr bool Enabled(bool requested, std::uint64_t title, unsigned version, unsigned rpx)
{
    return requested && title == 0x000500001012cf00ull && version == 16 && rpx == 0xefa75829;
}
bool RuntimeEnabled();
constexpr bool CanReuse(bool compiled, std::string_view preparedState, std::string_view actualState,
    std::string_view preparedVS, std::string_view actualVS, std::string_view preparedPS, std::string_view actualPS)
{
    return compiled && !preparedState.empty() && preparedState == actualState &&
        !preparedVS.empty() && preparedVS == actualVS && !preparedPS.empty() && preparedPS == actualPS;
}
constexpr std::uint64_t VertexBase(std::uint64_t code, std::uint64_t fetch, std::uint64_t imports,
    std::uint32_t vte, bool points, bool rects, bool halfZ)
{
    return code + fetch + imports + (vte ^ 0x43fu) + (points ? 71u : rects ? 13u : 0u) + (halfZ ? 0x1537u : 0u);
}
}

struct LatteContextRegister;
struct LatteDecompilerShader;
struct LatteFetchShader;
struct RevelationsPreviewShaders
{
    LatteFetchShader* fetch{};
    LatteDecompilerShader* vertex{};
    LatteDecompilerShader* pixel{};
    bool ownVertex{}, ownPixel{};
    std::uint32_t step0{}, step1{};
    std::vector<std::uint32_t> fetchCode, vertexCode, pixelCode;
    ~RevelationsPreviewShaders();
};
void LatteCP_PreviewUpcomingVulkanDraws(const std::function<bool(LatteContextRegister&)>& prepare);
std::shared_ptr<RevelationsPreviewShaders> LatteShader_CreateVulkanPreviewShaders(LatteContextRegister& context,
    std::vector<std::weak_ptr<RevelationsPreviewShaders>>& cache);
