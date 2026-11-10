#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"
#include "SplatoonLobbyLabels.h"
#include "PipelineBlendHash.h"
#include "YoshiAsyncPipeline.h"
#include "RevelationsVulkanTiming.h"
#include "RevelationsPipelinePreparation.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "RevelationsPipelineStateAudit.h"
#include <deque>
#include "util/helpers/StringBuf.h"
#include <openssl/sha.h>

#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/MarioKart8LobbyChannels.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/CafeSystem.h"
#include "Cemu/Debug/CompatibilityDiagnostics.h"
#include "imgui/imgui_impl_vulkan.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "config/ActiveSettings.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "util/helpers/helpers.h"

extern bool hasValidFramebufferAttached;

bool RevelationsVulkanTiming::RuntimeEnabled()
{
	static const bool requested = std::getenv("CEMU_RE_VK_TIMING") != nullptr;
	return Enabled(requested, CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
}

bool RevelationsPipelinePreparation::RuntimeEnabled()
{
	static const bool requested = std::getenv("CEMU_RE_DISABLE_PREPARATION") == nullptr;
	return Enabled(requested, CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
}

namespace
{
struct PreparedRevelationsPipeline
{
    uint64 hash{}, vsBase{};
    uint32 submittedFrame{};
    std::shared_ptr<RevelationsPreviewShaders> shaders;
    std::unique_ptr<PipelineInfo> info;
    std::shared_future<bool> ready;
    std::string state, vertexSource, pixelSource;
    std::chrono::steady_clock::time_point submitted;
    bool Wait() const
    {
        if (!ready.valid()) return false;
        try { return ready.get(); } catch (...) { return false; }
    }
    ~PreparedRevelationsPipeline()
    {
        if (ready.valid()) ready.wait();
        info.reset();
        shaders.reset();
    }
};
std::deque<std::unique_ptr<PreparedRevelationsPipeline>> s_preparedRevelationsPipelines;
std::vector<std::weak_ptr<RevelationsPreviewShaders>> s_previewShaderBundles;
unsigned s_preparedSubmitted{}, s_preparedUsed{}, s_preparedRejected{};
bool s_preparationTestDrained{};

struct RevelationPipelineMissTimer
{
    bool enabled;
    uint64 base, state;
    std::chrono::steady_clock::time_point start;
    RevelationPipelineMissTimer(uint64 base, uint64 state)
        : enabled(std::getenv("CEMU_RE_VK_CREATE_AUDIT") && RevelationsVulkanTiming::RuntimeEnabled()), base(base), state(state)
    {
        if (enabled) start = std::chrono::steady_clock::now();
    }
    ~RevelationPipelineMissTimer()
    {
        if (enabled)
            cemuLog_log(LogType::Force, "[RE:VKMISS] base={:016x} state={:016x} duration_ms={:.3f}", base, state,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
};

std::string PreparationState(const PipelineCompiler& compiler, const PipelineInfo& info, uint64 renderPass)
{
    std::ostringstream out;
    out << RevelationsPipelineReuse::CreationState(compiler, info, renderPass);
    out << compiler.descriptorSetLayoutCount << ';';
    for (const auto* shader : {info.vertexShader, info.geometryShader, info.pixelShader})
    {
        if (!shader) { out << "none;"; continue; }
        auto mapping = shader->resourceMapping;
        out << mapping.getTextureBaseBindingPoint() << ',' << mapping.getTextureCount() << ','
            << mapping.uniformVarsBufferBindingPoint << ',' << mapping.tfStorageBindingPoint << ';';
        for (const auto binding : mapping.uniformBuffersBindingPoint) out << binding << ',';
        out << ';';
    }
    for (unsigned i = 0; i < VulkanRendererConst::SHADER_STAGE_INDEX_COUNT; ++i)
    {
        out << info.dynamicOffsetInfo.hasUniformVar[i] << ',' << info.dynamicOffsetInfo.hasUniformBuffers[i] << ';';
        for (const auto index : info.dynamicOffsetInfo.list_uniformBuffers[i]) out << unsigned(index) << ',';
        out << ';';
    }
    return out.str();
}
}

void VulkanRenderer::RevelationsClearPreparedPipelines()
{
    if (!s_preparedRevelationsPipelines.empty() || s_preparedSubmitted)
        cemuLog_log(LogType::Force, "[RE:VKPREP] event=summary submitted={} used={} rejected={} unused={}",
            s_preparedSubmitted, s_preparedUsed, s_preparedRejected, s_preparedRevelationsPipelines.size());
    s_preparedRevelationsPipelines.clear();
    s_previewShaderBundles.clear();
    s_preparedSubmitted = s_preparedUsed = s_preparedRejected = 0;
}

namespace
{
	bool IsMN9TraceTarget()
	{
		return CafeSystem::GetForegroundTitleId() == 0x00050000101C9600ull &&
			CafeSystem::GetForegroundTitleVersion() == 16 &&
			CafeSystem::GetRPXHashUpdated() == 0x73E4FFC8;
	}

	bool IsMN9BrokenBoxSample(const LatteDecompilerShader* shader, uint32 textureUnit, const LatteTexture* texture)
	{
		return IsMN9TraceTarget() && shader && texture &&
			shader->shaderType == LatteConst::ShaderType::Pixel &&
			shader->baseHash == 0xe9a04c8c336eaac6ull &&
			textureUnit == 3 &&
			texture->width == 512 && texture->height == 512 &&
			texture->mipLevels == 10 &&
			texture->format == Latte::E_GX2SURFFMT::BC3_SRGB;
	}

	std::optional<sint64> GetCompatibilityDrawTraceSample()
	{
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		if (!CompatibilityDiagnostics::IsEnabled("draw", titleId))
			return std::nullopt;

		static const sint64 s_configuredStartTime = CompatibilityDiagnostics::GetInteger("CEMU_COMPAT_DRAW_START_MS", 68000);
		static const sint64 s_configuredInterval = std::max<sint64>(CompatibilityDiagnostics::GetInteger("CEMU_COMPAT_DRAW_INTERVAL_MS", 4000), 1);
		static const size_t s_configuredSampleCount = (size_t)std::max<sint64>(CompatibilityDiagnostics::GetInteger("CEMU_COMPAT_DRAW_COUNT", 5), 1);
		static uint64 s_traceTitleId = 0;
		static size_t s_nextSample = 0;
		static uint32 s_targetFrame = std::numeric_limits<uint32>::max();
		const sint64 elapsedMilliseconds = CompatibilityDiagnostics::ElapsedMilliseconds();
		if (s_traceTitleId != titleId)
		{
			s_traceTitleId = titleId;
			s_nextSample = 0;
			s_targetFrame = std::numeric_limits<uint32>::max();
		}

		if (s_targetFrame != std::numeric_limits<uint32>::max() && LatteGPUState.frameCounter > s_targetFrame)
		{
			s_targetFrame = std::numeric_limits<uint32>::max();
			s_nextSample++;
		}
		if (s_nextSample >= s_configuredSampleCount)
			return std::nullopt;
		const sint64 sampleTime = s_configuredStartTime + (sint64)s_nextSample * s_configuredInterval;
		if (s_targetFrame == std::numeric_limits<uint32>::max() && elapsedMilliseconds >= sampleTime)
		{
			s_targetFrame = LatteGPUState.frameCounter + 1;
			cemuLog_log(LogType::Force, "[COMPAT:DRAW] backend=Vulkan event=select sample_ms={} frame={}", sampleTime, s_targetFrame);
		}
		return LatteGPUState.frameCounter == s_targetFrame ? std::optional<sint64>{ sampleTime } : std::nullopt;
	}
}

// includes only states that may change during minimal drawcalls
uint64 VulkanRenderer::draw_calculateMinimalGraphicsPipelineHash(const LatteFetchShader* fetchShader, const LatteContextRegister& lcr)
{
	uint64 stateHash = 0;

	// fetch shader
	for (auto& group : fetchShader->bufferGroups)
	{
		uint32 bufferStride = group.getCurrentBufferStride(lcr.GetRawView());
		stateHash = std::rotl<uint64>(stateHash, 7);
		stateHash += bufferStride * 3;
	}

	stateHash += fetchShader->getVkPipelineHashFragment();
	stateHash = std::rotl<uint64>(stateHash, 7);

	stateHash += lcr.GetRawView()[mmVGT_PRIMITIVE_TYPE];
	stateHash = std::rotl<uint64>(stateHash, 7);

	stateHash += lcr.GetRawView()[mmVGT_STRMOUT_EN];
	stateHash = std::rotl<uint64>(stateHash, 7);

	if(lcr.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL())
		stateHash += 0x333333;

	return stateHash;
}

uint64 VulkanRenderer::draw_calculateGraphicsPipelineHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const VKRObjectRenderPass* renderPassObj, const LatteContextRegister& lcr)
{
	// note: vertexShader references a fetchShader (vertexShader->fetchShader) but it's not necessarily the one that is currently active
	// this is because we try to separate dynamic state (mainly attribute offsets) from the actual attribute data layout and mapping (types and slots)
	// on Vulkan this causes issues because we bake the attribute offsets, which may not match vertexShader->compatibleFetchShader, into the pipeline
	// To avoid issues always use the active fetch shader. Not the one associated with the vertexShader object
	// note 2:
	// there is a secondary issue where we dont store all fetch shaders into the pipeline cache (only a single fetch shader is tied to each stored vertex shader)
	// but we can probably trust drivers to not require pipeline recompilation if only the offsets differ
	// An alternative would be to use VK_EXT_vertex_input_dynamic_state but it comes with minor overhead
	// Regardless, the extension is not well supported as of writing this (July 2021, only 10% of GPUs support it on Windows. Nvidia only)

	cemu_assert_debug(vertexShader->compatibleFetchShader->key == fetchShader->key); // fetch shaders must be layout compatible, but may have different offsets

	uint64 stateHash;
	stateHash = draw_calculateMinimalGraphicsPipelineHash(fetchShader, lcr);
	stateHash = (stateHash >> 8) + (stateHash * 0x370531ull) % 0x7F980D3BF9B4639Dull;

	uint32* ctxRegister = lcr.GetRawView();

	if (vertexShader)
		stateHash += vertexShader->baseHash;

	stateHash = std::rotl<uint64>(stateHash, 13);

	if (geometryShader)
		stateHash += geometryShader->baseHash;

	stateHash = std::rotl<uint64>(stateHash, 13);

	if (pixelShader)
		stateHash += pixelShader->baseHash + pixelShader->auxHash;

	stateHash = std::rotl<uint64>(stateHash, 13);

	uint32 polygonCtrl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
	stateHash += polygonCtrl;
	stateHash = std::rotl<uint64>(stateHash, 7);

	stateHash += ctxRegister[Latte::REGADDR::PA_CL_CLIP_CNTL];
	stateHash = std::rotl<uint64>(stateHash, 7);

	const auto colorControlReg = ctxRegister[Latte::REGADDR::CB_COLOR_CONTROL];
	stateHash += colorControlReg;

	stateHash += ctxRegister[Latte::REGADDR::CB_TARGET_MASK];

	const uint32 blendEnableMask = (colorControlReg >> 8) & 0xFF;
	if (blendEnableMask)
	{
		for (auto i = 0; i < 8; ++i)
		{
			if (((blendEnableMask & (1 << i))) == 0)
				continue;
			stateHash = std::rotl<uint64>(stateHash, 7);
			stateHash += ctxRegister[Latte::REGADDR::CB_BLEND0_CONTROL + i];
		}
	}

	stateHash += renderPassObj->m_hashForPipeline;

	uint32 depthControl = ctxRegister[Latte::REGADDR::DB_DEPTH_CONTROL];
	bool stencilTestEnable = depthControl & 1;
	if (stencilTestEnable)
	{
		stateHash += ctxRegister[mmDB_STENCILREFMASK];
		stateHash = std::rotl<uint64>(stateHash, 17);
		if(depthControl & (1<<7)) // back stencil enable
		{
			stateHash += ctxRegister[mmDB_STENCILREFMASK_BF];
			stateHash = std::rotl<uint64>(stateHash, 13);
		}
	}
	else
	{
		// zero out stencil related bits (8-31)
		depthControl &= 0xFF;
	}

	stateHash = std::rotl<uint64>(stateHash, 17);
	stateHash += depthControl;

	// polygon offset
	if (polygonCtrl & (1 << 11))
	{
		// front offset enabled
		stateHash += 0x1111;
	}

	// Keep blend changes from cancelling other additive contributions to the key.
	// Wonderful 101 otherwise reuses an alpha-blend pipeline for additive credits.
	for (uint32 i = 0; i < 8; i++)
	{
		if (blendEnableMask & (1u << i))
			stateHash = PipelineBlendHash::Mix(stateHash, lcr.CB_BLENDN_CONTROL[i].getRawValue());
	}

	return stateHash;
}

void VulkanRenderer::draw_debugPipelineHashState()
{
	cemu_assert_debug(false);
}

PipelineInfo* VulkanRenderer::draw_getCachedPipeline()
{
	// todo - optimize m_pipeline_info_cache away and store directly in vk vertex shader
	const auto fetchShader = LatteSHRC_GetActiveFetchShader();
	const auto vertexShader = LatteSHRC_GetActiveVertexShader();
	const auto it = m_pipeline_info_cache.find(vertexShader->baseHash);
	if (it == m_pipeline_info_cache.cend())
		return nullptr;

	const auto geometryShader = LatteSHRC_GetActiveGeometryShader();
	const auto pixelShader = LatteSHRC_GetActivePixelShader();
	auto cachedFboVk = (CachedFBOVk*)m_state.activeFBO;

	const uint64 stateHash = draw_calculateGraphicsPipelineHash(fetchShader, vertexShader, geometryShader, pixelShader, cachedFboVk->GetRenderPassObj(), LatteGPUState.contextNew);

	const auto innerit = it->second.find(stateHash);
	if (innerit == it->second.cend())
		return nullptr;

	return innerit->second;
}

void VulkanRenderer::unregisterGraphicsPipeline(PipelineInfo* pipelineInfo)
{
	bool removedFromCache = false;
	for (auto& topMapItr : m_pipeline_info_cache)
	{
		auto& subMap = topMapItr.second;
		for (auto it = subMap.cbegin(); it != subMap.cend();)
		{
			if (it->second == pipelineInfo)
			{
				subMap.erase(it);
				removedFromCache = true;
				break;
			}
			++it;
		}
		if (removedFromCache)
			break;
	}
}

// make a guess if a pipeline is not essential
// non-essential means that skipping these drawcalls shouldn't lead to permanently corrupted graphics
bool VulkanRenderer::IsAsyncPipelineAllowed(uint32 numIndices)
{
	// frame debuggers dont handle async well (as of 2020)
	if (IsTracingToolEnabled())
		return false;

	CachedFBOVk* currentFBO = m_state.activeFBO;
	auto fboExtend = currentFBO->GetExtend();
	if (YoshiAsyncPipeline::Matches(CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) &&
		YoshiAsyncPipeline::PreserveDraw(fboExtend.width, fboExtend.height, numIndices, currentFBO->hasDepthBuffer()))
		return false;
	if (SplatoonLobbyLabels::MatchesTitle(CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()))
	{
		const auto* vs = LatteSHRC_GetActiveVertexShader();
		const auto* ps = LatteSHRC_GetActivePixelShader();
		if (vs && ps && SplatoonLobbyLabels::IsPersistentFontDraw(
			vs->baseHash, ps->baseHash, fboExtend.width, fboExtend.height))
		{
			// These one-time draws are compressed into cached labels, never redrawn.
			cemuLog_logOnce(LogType::Force, "Splatoon: preserving persistent lobby font draws");
			return false;
		}
	}

	if (fboExtend.width == 1600 && fboExtend.height == 1600)
		return false; // Splatoon ink mechanics use 1600x1600 R8 and R8G8 framebuffers, this resolution is rare enough that we can just blacklist it globally

	if (currentFBO->hasDepthBuffer())
		return true; // aggressive filter but seems to work well so far

	// small index count (3,4,5,6) is often associated with full-viewport quads (which are considered essential due to often being used to generate persistent textures)
	if (numIndices <= 6)
	{

		return false;
	}

	return true;
}

// create graphics pipeline for current state
PipelineInfo* VulkanRenderer::draw_createGraphicsPipeline(uint32 indexCount)
{
	const bool traceMN9 = IsMN9TraceTarget();
	const HRTick pipelineStart = traceMN9 ? HighResolutionTimer::now().getTick() : 0;
	const auto fetchShader = LatteSHRC_GetActiveFetchShader();
	const auto vertexShader = LatteSHRC_GetActiveVertexShader();
	const auto geometryShader = LatteSHRC_GetActiveGeometryShader();
	const auto pixelShader = LatteSHRC_GetActivePixelShader();
	auto cachedFboVk = (CachedFBOVk*)m_state.activeFBO;

	uint64 minimalStateHash = draw_calculateMinimalGraphicsPipelineHash(fetchShader, LatteGPUState.contextNew);
	uint64 pipelineHash = draw_calculateGraphicsPipelineHash(fetchShader, vertexShader, geometryShader, pixelShader, cachedFboVk->GetRenderPassObj(), LatteGPUState.contextNew);
	RevelationPipelineMissTimer missTimer(vertexShader->baseHash, pipelineHash);

	// create PipelineInfo
	auto vkFBO = (CachedFBOVk*)(VulkanRenderer::GetInstance()->m_state.activeFBO);
	PipelineInfo* pipelineInfo = new PipelineInfo(minimalStateHash, pipelineHash, fetchShader, vertexShader, pixelShader, geometryShader);

	// register pipeline
	uint64 vsBaseHash = vertexShader->baseHash;
	auto it = m_pipeline_info_cache.emplace(vsBaseHash, robin_hood::unordered_flat_map<uint64, PipelineInfo*>());
	auto& cache_map = it.first->second;
	cache_map.emplace(pipelineHash, pipelineInfo);

	// init pipeline compiler
	PipelineCompiler* pipelineCompiler = new PipelineCompiler();

	bool requiresRobustBufferAccess = PipelineCompiler::CalcRobustBufferAccessRequirement(vertexShader, pixelShader, geometryShader);
	pipelineCompiler->InitFromCurrentGPUState(pipelineInfo, LatteGPUState.contextNew, vkFBO->GetRenderPassObj(), requiresRobustBufferAccess);
	pipelineCompiler->TrackAsCached(vsBaseHash, pipelineHash);
	if (std::getenv("CEMU_RE_VK_CREATE_AUDIT") && RevelationsVulkanTiming::RuntimeEnabled())
	{
		const auto state = PreparationState(*pipelineCompiler, *pipelineInfo, vkFBO->GetRenderPassObj()->m_hashForPipeline);
		uint8 digest[SHA256_DIGEST_LENGTH];
		SHA256(reinterpret_cast<const uint8*>(state.data()), state.size(), digest);
		std::string fingerprint;
		for (const auto byte : digest) fingerprint += fmt::format("{:02x}", byte);
		cemuLog_log(LogType::Force, "[RE:VKCREATE] base={:016x} state={:016x} fingerprint={}", vsBaseHash, pipelineHash, fingerprint);
	}

	const bool prepareRE = RevelationsPipelinePreparation::RuntimeEnabled() &&
		!s_preparationTestDrained &&
		!ActiveSettings::AsyncShaderCompileEnabled() && !geometryShader &&
		!vertexShader->isCustomShader && pixelShader && !pixelShader->isCustomShader &&
		LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE() != LattePrimitiveMode::RECTS &&
		LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] == 0;
	if (prepareRE)
	{
		const auto actualState = PreparationState(*pipelineCompiler, *pipelineInfo, vkFBO->GetRenderPassObj()->m_hashForPipeline);
		const auto* actualVS = static_cast<RendererShaderVk*>(vertexShader->shader);
		const auto* actualPS = static_cast<RendererShaderVk*>(pixelShader->shader);
		for (auto it = s_preparedRevelationsPipelines.begin(); it != s_preparedRevelationsPipelines.end(); ++it)
		{
			auto& entry = **it;
			if (entry.hash != pipelineHash || entry.vsBase != vsBaseHash) continue;
			const auto waitStart = std::chrono::steady_clock::now();
			const bool compiled = entry.Wait();
			const bool match = RevelationsPipelinePreparation::CanReuse(compiled, entry.state, actualState,
				entry.vertexSource, actualVS->GetPreparationSource(), entry.pixelSource, actualPS->GetPreparationSource());
			if (match)
			{
				if (!vertexShader->shader->IsCompiled()) vertexShader->shader->PreponeCompilation(true);
				if (!pixelShader->shader->IsCompiled()) pixelShader->shader->PreponeCompilation(true);
				if (pipelineInfo->vertexShaderVk->GetShaderModule() && pipelineInfo->pixelShaderVk->GetShaderModule())
				{
					// Only the compiled handle/layout bundle moves; actual draw metadata stays authoritative.
					std::swap(pipelineInfo->m_vkrObjPipeline, entry.info->m_vkrObjPipeline);
					++s_preparedUsed;
					cemuLog_log(LogType::Force, "[RE:VKPREP] event=use pipeline={:016x} validated=true wait_ms={:.3f} lead_ms={:.3f}",
						pipelineHash, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count(),
						std::chrono::duration<double, std::milli>(waitStart - entry.submitted).count());
					delete pipelineCompiler;
					s_preparedRevelationsPipelines.erase(it);
					return pipelineInfo;
				}
			}
			++s_preparedRejected;
			cemuLog_log(LogType::Force, "[RE:VKPREP] event=reject pipeline={:016x} compiled={} state_match={}", pipelineHash, compiled, match);
			s_preparedRevelationsPipelines.erase(it);
			break;
		}

		// Old completed hints must not occupy the queue when a new area starts.
		for (auto it = s_preparedRevelationsPipelines.begin(); it != s_preparedRevelationsPipelines.end();)
		{
			const auto& entry = **it;
			const bool completed = entry.ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			if (RevelationsPipelinePreparation::RetireUnused(completed, entry.submittedFrame, LatteGPUState.frameCounter))
				it = s_preparedRevelationsPipelines.erase(it);
			else ++it;
		}
		// At capacity, retire only completed unused hints; never destroy a worker's input.
		while (s_preparedRevelationsPipelines.size() >= RevelationsPipelinePreparation::MaxPending)
		{
			auto it = std::find_if(s_preparedRevelationsPipelines.begin(), s_preparedRevelationsPipelines.end(), [](const auto& entry) {
				return entry->ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			});
			if (it == s_preparedRevelationsPipelines.end()) break;
			s_preparedRevelationsPipelines.erase(it);
		}
		const auto currentReady = PipelineCompiler::CompileThreadPool_QueuePreparation(pipelineCompiler);
		const auto reference = std::make_unique<LatteContextRegister>(LatteGPUState.contextNew);
		LatteCP_PreviewUpcomingVulkanDraws([&](LatteContextRegister& context)
		{
			if (s_preparedRevelationsPipelines.size() >= RevelationsPipelinePreparation::MaxPending) return false;
			auto* registers = context.GetRawView();
			const auto* original = reference->GetRawView();
			if (context.VGT_GS_MODE.get_MODE() != Latte::LATTE_VGT_GS_MODE::E_MODE::OFF || registers[mmVGT_STRMOUT_EN] ||
				std::memcmp(registers + 0xa000, original + 0xa000, 0x40 * sizeof(uint32))) return true;
			auto entry = std::make_unique<PreparedRevelationsPipeline>();
			entry->shaders = LatteShader_CreateVulkanPreviewShaders(context, s_previewShaderBundles);
			if (!entry->shaders) return true;
			auto& shaders = *entry->shaders;
			entry->hash = draw_calculateGraphicsPipelineHash(shaders.fetch, shaders.vertex, nullptr, shaders.pixel, vkFBO->GetRenderPassObj(), context);
			entry->vsBase = shaders.vertex->baseHash;
			const auto cached = m_pipeline_info_cache.find(entry->vsBase);
			if ((cached != m_pipeline_info_cache.end() && cached->second.contains(entry->hash)) ||
				std::any_of(s_preparedRevelationsPipelines.begin(), s_preparedRevelationsPipelines.end(), [&](const auto& other) { return other->hash == entry->hash && other->vsBase == entry->vsBase; })) return true;
			const auto prepareStage = [](LatteDecompilerShader* shader, RendererShader::ShaderType type) -> std::string
			{
				if (!shader->shader)
				{
					if (!shader->strBuf_shaderSource) return {};
					shader->shader = new RendererShaderVk(type, shader->baseHash, shader->auxHash, false, false, shader->strBuf_shaderSource->c_str());
				}
				return static_cast<RendererShaderVk*>(shader->shader)->GetPreparationSource();
			};
			entry->vertexSource = prepareStage(shaders.vertex, RendererShader::ShaderType::kVertex);
			entry->pixelSource = prepareStage(shaders.pixel, RendererShader::ShaderType::kFragment);
			if (entry->vertexSource.empty() || entry->pixelSource.empty()) return true;
			entry->info = std::make_unique<PipelineInfo>(draw_calculateMinimalGraphicsPipelineHash(shaders.fetch, context), entry->hash,
				shaders.fetch, shaders.vertex, shaders.pixel, nullptr);
			auto compiler = std::make_unique<PipelineCompiler>();
			if (!compiler->InitFromCurrentGPUState(entry->info.get(), context, vkFBO->GetRenderPassObj(),
				PipelineCompiler::CalcRobustBufferAccessRequirement(shaders.vertex, shaders.pixel, nullptr)))
			{
				// Failed initialization has not acquired these references yet.
				compiler->m_vkrObjPipeline = nullptr;
				compiler->m_renderPassObj = nullptr;
				return true;
			}
			entry->state = PreparationState(*compiler, *entry->info, vkFBO->GetRenderPassObj()->m_hashForPipeline);
			entry->submitted = std::chrono::steady_clock::now();
			entry->submittedFrame = LatteGPUState.frameCounter;
			entry->ready = PipelineCompiler::CompileThreadPool_QueuePreparation(compiler.release());
			++s_preparedSubmitted;
			cemuLog_log(LogType::Force, "[RE:VKPREP] event=submit pipeline={:016x} pending={}", entry->hash, s_preparedRevelationsPipelines.size() + 1);
			s_preparedRevelationsPipelines.push_back(std::move(entry));
			return true;
		});
		if (!currentReady.get()) throw std::runtime_error("Revelations current pipeline compilation failed");
		return pipelineInfo;
	}

	// use heuristics based on parameter patterns to determine if the current drawcall is essential (non-skipable)
	bool allowAsyncCompile = false;
	if (ActiveSettings::AsyncShaderCompileEnabled())
		allowAsyncCompile = IsAsyncPipelineAllowed(indexCount);

	if (allowAsyncCompile)
	{
		// even when async is allowed, attempt synchronous creation first (which will immediately fail if the pipeline is not cached)
		if (pipelineCompiler->Compile(false, true, true) == false)
		{
			// shaders or pipeline not cached -> asynchronous compilation
			PipelineCompiler::CompileThreadPool_QueueCompilation(pipelineCompiler);
		}
		else
		{
			delete pipelineCompiler;
		}
	}
	else
	{
		// synchronous compilation
		pipelineCompiler->Compile(true, true, true);
		delete pipelineCompiler;
	}
	if (traceMN9)
	{
		const double pipelineMs = HighResolutionTimer::getTimeDiff(pipelineStart, HighResolutionTimer::now().getTick()) * 1000.0;
		if (pipelineMs >= 25.0)
		{
			cemuLog_log(LogType::Force,
				"[COMPAT:MN9] stall=graphics-pipeline duration_ms={:.3f} async_allowed={} indices={} pipeline={:016x} minimal={:016x} vs={:016x} ps={:016x} gs={:016x}",
				pipelineMs, allowAsyncCompile, indexCount, pipelineHash, minimalStateHash,
				vertexShader ? vertexShader->baseHash : 0,
				pixelShader ? pixelShader->baseHash : 0,
				geometryShader ? geometryShader->baseHash : 0);
		}
	}

	return pipelineInfo;
}

PipelineInfo* VulkanRenderer::draw_getOrCreateGraphicsPipeline(uint32 indexCount)
{
	auto cache_object = draw_getCachedPipeline();
	if (cache_object != nullptr)
	{

#ifdef CEMU_DEBUG_ASSERT
		cemu_assert_debug(cache_object->vertexShader == LatteSHRC_GetActiveVertexShader());
		cemu_assert_debug(cache_object->geometryShader == LatteSHRC_GetActiveGeometryShader());
		cemu_assert_debug(cache_object->pixelShader == LatteSHRC_GetActivePixelShader());
		if (cache_object->fetchShader->key != LatteSHRC_GetActiveFetchShader()->key ||
			cache_object->fetchShader->vkPipelineHashFragment != LatteSHRC_GetActiveFetchShader()->vkPipelineHashFragment)
		{
			debug_printf("Incompatible fetch shader %p %p\n", cache_object->fetchShader, LatteSHRC_GetActiveFetchShader());
			assert_dbg();
		}
		uint64 calcMinimalHash = draw_calculateMinimalGraphicsPipelineHash(LatteSHRC_GetActiveFetchShader(), LatteGPUState.contextNew);
		auto currentPrimitiveMode = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
		cemu_assert_debug(cache_object->primitiveMode == currentPrimitiveMode);
		cemu_assert_debug(cache_object->minimalStateHash == calcMinimalHash);
#endif
		return cache_object;
	}
	//draw_debugPipelineHashState();

	return draw_createGraphicsPipeline(indexCount);
}

Renderer::IndexAllocation VulkanRenderer::indexData_reserveIndexMemory(uint32 size)
{
	VKRSynchronizedHeapAllocator::AllocatorReservation* resv = memoryManager->GetIndexAllocator().AllocateBufferMemory(size, 32);
	return { resv->memPtr, resv };
}

void VulkanRenderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
	memoryManager->GetIndexAllocator().FreeReservation((VKRSynchronizedHeapAllocator::AllocatorReservation*)allocation.rendererInternal);
}

void VulkanRenderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
	memoryManager->GetIndexAllocator().FlushReservation((VKRSynchronizedHeapAllocator::AllocatorReservation*)allocation.rendererInternal);
}

float s_vkUniformData[512 * 4];

uint32 VulkanRenderer::uniformData_uploadUniformDataBufferGetOffset(std::span<uint8> data)
{
	const uint32 bufferAlignmentM1 = std::max(m_featureControl.limits.minUniformBufferOffsetAlignment, m_featureControl.limits.nonCoherentAtomSize) - 1;
	const uint32 uniformSize = ((uint32)data.size() + bufferAlignmentM1) & ~bufferAlignmentM1;

	auto waitWhileCondition = [&](std::function<bool()> condition) {
		while (condition())
		{
			if (m_commandBufferSyncIndex == m_commandBufferIndex)
			{
				if (m_cmdBufferUniformRingbufIndices[m_commandBufferIndex] != m_uniformVarBufferReadIndex)
				{
					draw_endRenderPass();
					SubmitCommandBuffer();
				}
				else
				{
					// submitting work would not change readIndex, so there's no way for conditions based on it to change
					cemuLog_log(LogType::Force, "draw call overflowed and corrupted uniform ringbuffer. expect visual corruption");
					cemu_assert_suspicious();
					break;
				}
			}
			WaitForNextFinishedCommandBuffer();
		}
	};

	// wrap around if it doesnt fit consecutively
	if (m_uniformVarBufferWriteIndex + uniformSize > UNIFORMVAR_RINGBUFFER_SIZE)
	{
		waitWhileCondition([&]() {
			return m_uniformVarBufferReadIndex > m_uniformVarBufferWriteIndex || m_uniformVarBufferReadIndex == 0;
		});
		m_uniformVarBufferWriteIndex = 0;
	}

	auto ringBufRemaining = [&]() {
		ssize_t ringBufferUsedBytes = (ssize_t)m_uniformVarBufferWriteIndex - m_uniformVarBufferReadIndex;
		if (ringBufferUsedBytes < 0)
			ringBufferUsedBytes += UNIFORMVAR_RINGBUFFER_SIZE;
		return UNIFORMVAR_RINGBUFFER_SIZE - 1 - ringBufferUsedBytes;
	};
	waitWhileCondition([&]() {
		return ringBufRemaining() < uniformSize;
	});

	const uint32 uniformOffset = m_uniformVarBufferWriteIndex;
	memcpy(m_uniformVarBufferPtr + uniformOffset, data.data(), data.size());
	m_uniformVarBufferWriteIndex += uniformSize;
	// flush if not coherent
	if (!m_uniformVarBufferMemoryIsCoherent)
	{
		VkMappedMemoryRange flushedRange{};
		flushedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		flushedRange.memory = m_uniformVarBufferMemory;
		flushedRange.offset = uniformOffset;
		flushedRange.size = uniformSize;
		vkFlushMappedMemoryRanges(m_logicalDevice, 1, &flushedRange);
	}
	return uniformOffset;
}

void VulkanRenderer::uniformData_updateUniformVars(uint32 shaderStageIndex, LatteDecompilerShader* shader)
{
	auto GET_UNIFORM_DATA_PTR = [](size_t index) { return s_vkUniformData + (index / 4); };

	sint32 shaderAluConst;

	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		shaderAluConst = 0x400;
		break;
	case LatteConst::ShaderType::Pixel:
		shaderAluConst = 0;
		break;
	case LatteConst::ShaderType::Geometry:
		shaderAluConst = 0; // geometry shader has no ALU const
		break;
	default:
		UNREACHABLE;
	}

	if (shader->resourceMapping.uniformVarsBufferBindingPoint >= 0)
	{
		if (shader->uniform.list_ufTexRescale.empty() == false)
		{
			for (auto& entry : shader->uniform.list_ufTexRescale)
			{
				float* xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
				memcpy(entry.currentValue, xyScale, sizeof(float) * 2);
				memcpy(GET_UNIFORM_DATA_PTR(entry.uniformLocation), xyScale, sizeof(float) * 2);
			}
		}
		if (shader->uniform.loc_alphaTestRef >= 0)
		{
			*GET_UNIFORM_DATA_PTR(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
		}
		if (shader->uniform.loc_pointSize >= 0)
		{
			const auto& pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
			float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
			if (pointWidth == 0.0f)
				pointWidth = 1.0f / 8.0f; // minimum size
			*GET_UNIFORM_DATA_PTR(shader->uniform.loc_pointSize) = pointWidth;
		}
		if (shader->uniform.loc_remapped >= 0)
		{
			LatteBufferCache_LoadRemappedUniforms(shader, GET_UNIFORM_DATA_PTR(shader->uniform.loc_remapped));
		}
		if (shader->uniform.loc_uniformRegister >= 0)
		{
			uint32* uniformRegData = (uint32*)(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst);
			memcpy(GET_UNIFORM_DATA_PTR(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
		}
		if (shader->uniform.loc_loopConsts >= 0)
		{
			const uint32 stageOffset = shader->shaderType == LatteConst::ShaderType::Vertex ? 32 :
				(shader->shaderType == LatteConst::ShaderType::Pixel ? 0 : 64);
			memcpy(GET_UNIFORM_DATA_PTR(shader->uniform.loc_loopConsts),
				LatteGPUState.contextRegister + LATTE_REG_BASE_LOOP_CONST + stageOffset, 128);
		}
		if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
		{
			sint32 viewportWidth;
			sint32 viewportHeight;
			LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight); // always call after _updateViewport()
			float* v = GET_UNIFORM_DATA_PTR(shader->uniform.loc_windowSpaceToClipSpaceTransform);
			v[0] = 2.0f / (float)viewportWidth;
			v[1] = 2.0f / (float)viewportHeight;
		}
		if (shader->uniform.loc_fragCoordScale >= 0)
		{
			LatteMRT::GetCurrentFragCoordScale(GET_UNIFORM_DATA_PTR(shader->uniform.loc_fragCoordScale));
		}
		if (shader->uniform.loc_verticesPerInstance >= 0)
		{
			*(int*)GET_UNIFORM_DATA_PTR(shader->uniform.loc_verticesPerInstance) = m_streamoutState.verticesPerInstance;
			for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
			{
				if (shader->uniform.loc_streamoutBufferBase[b] >= 0)
				{
					uint32 base = m_streamoutState.buffer[b].ringBufferOffset;
					if (shaderStageIndex == VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX)
						base -= m_streamoutState.vertexIndexBase * (LatteGPUState.contextRegister[mmVGT_STRMOUT_VTX_STRIDE_0 + b * 4] << 2);
					*(uint32*)GET_UNIFORM_DATA_PTR(shader->uniform.loc_streamoutBufferBase[b]) = base;
				}
			}
		}
		dynamicOffsetInfo.uniformVarBufferOffset[shaderStageIndex] = uniformData_uploadUniformDataBufferGetOffset({(uint8*)s_vkUniformData, shader->uniform.uniformRangeSize});
	}
}

void VulkanRenderer::draw_prepareDynamicOffsetsForDescriptorSet(uint32 shaderStageIndex, uint32* dynamicOffsets,
	sint32& numDynOffsets,
	const PipelineInfo* pipeline_info)
{
	numDynOffsets = 0;
	if (pipeline_info->dynamicOffsetInfo.hasUniformVar[shaderStageIndex])
	{
		dynamicOffsets[0] = dynamicOffsetInfo.uniformVarBufferOffset[shaderStageIndex];
		numDynOffsets++;
	}
	if (pipeline_info->dynamicOffsetInfo.hasUniformBuffers[shaderStageIndex])
	{
		for (auto& itr : pipeline_info->dynamicOffsetInfo.list_uniformBuffers[shaderStageIndex])
		{
			dynamicOffsets[numDynOffsets] = dynamicOffsetInfo.shaderUB[shaderStageIndex].uniformBufferOffset[itr];
			numDynOffsets++;
		}
	}
}

uint64 VulkanRenderer::GetDescriptorSetStateHash(LatteDecompilerShader* shader)
{
	uint64 hash = 0;

	const sint32 textureCount = shader->resourceMapping.getTextureCount();
	for (int i = 0; i < textureCount; ++i)
	{
		const auto relative_textureUnit = shader->resourceMapping.getTextureUnitFromBindingPoint(i);
		auto hostTextureUnit = relative_textureUnit;
		auto textureDim = shader->textureUnitDim[relative_textureUnit];
		auto texUnitRegIndex = hostTextureUnit * 7;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
			break;
		case LatteConst::ShaderType::Pixel:
			hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
			break;
		case LatteConst::ShaderType::Geometry:
			hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
			break;
		default:
			UNREACHABLE;
		}

		auto texture = m_state.boundTexture[hostTextureUnit];
		if (!texture)
			continue;

		const uint32 word4 = LatteGPUState.contextRegister[texUnitRegIndex + 4];

		uint32 samplerIndex = shader->textureUnitSamplerAssignment[relative_textureUnit];
		if (samplerIndex != LATTE_DECOMPILER_SAMPLER_NONE)
		{
			samplerIndex += LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
			hash += LatteGPUState.contextRegister[Latte::REGADDR::SQ_TEX_SAMPLER_WORD0_0 + samplerIndex * 3 + 0];
			hash = std::rotl<uint64>(hash, 7);
			hash += LatteGPUState.contextRegister[Latte::REGADDR::SQ_TEX_SAMPLER_WORD0_0 + samplerIndex * 3 + 1];
			hash = std::rotl<uint64>(hash, 7);
			hash += LatteGPUState.contextRegister[Latte::REGADDR::SQ_TEX_SAMPLER_WORD0_0 + samplerIndex * 3 + 2];
			hash = std::rotl<uint64>(hash, 7);
		}
		hash = std::rotl<uint64>(hash, 7);
		// hash view id + swizzle mask
		hash += (uint64)texture->GetUniqueId();
		hash = std::rotr<uint64>(hash, 21);
		hash += (uint64)(word4 & 0x0FFF0000);
	}

	return hash;
}

VkDescriptorSetInfo* VulkanRenderer::draw_getOrCreateDescriptorSet(PipelineInfo* pipeline_info, LatteDecompilerShader* shader)
{
	const uint64 stateHash = GetDescriptorSetStateHash(shader);

	VkDescriptorSetLayout descriptor_set_layout;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
	{
		const auto it = pipeline_info->vertex_ds_cache.find(stateHash);
		if (it != pipeline_info->vertex_ds_cache.cend())
			return it->second;
		descriptor_set_layout = pipeline_info->m_vkrObjPipeline->m_vertexDSL;
		break;
	}
	case LatteConst::ShaderType::Pixel:
	{
		const auto it = pipeline_info->pixel_ds_cache.find(stateHash);
		if (it != pipeline_info->pixel_ds_cache.cend())
			return it->second;
		descriptor_set_layout = pipeline_info->m_vkrObjPipeline->m_pixelDSL;
		break;
	}
	case LatteConst::ShaderType::Geometry:
	{
		const auto it = pipeline_info->geometry_ds_cache.find(stateHash);
		if (it != pipeline_info->geometry_ds_cache.cend())
			return it->second;
		descriptor_set_layout = pipeline_info->m_vkrObjPipeline->m_geometryDSL;
		break;
	}
	default:
		UNREACHABLE;
	}

	// create new descriptor set
	VkDescriptorSetInfo* dsInfo = new VkDescriptorSetInfo();
	dsInfo->stateHash = stateHash;
	dsInfo->shaderType = shader->shaderType;
	dsInfo->pipeline_info = pipeline_info;

	dsInfo->m_vkObjDescriptorSet = new VKRObjectDescriptorSet();
	auto vkObjDS = dsInfo->m_vkObjDescriptorSet;

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &descriptor_set_layout;

	VkDescriptorSet result;
	if (vkAllocateDescriptorSets(m_logicalDevice, &allocInfo, &result) != VK_SUCCESS)
	{
		UnrecoverableError(fmt::format("Failed to allocate descriptor sets. Currently allocated: Descriptors={} TextureSamplers={} DynUniformBuffers={} StorageBuffers={}",
			performanceMonitor.vk.numDescriptorSets.get(),
			performanceMonitor.vk.numDescriptorSamplerTextures.get(),
			performanceMonitor.vk.numDescriptorDynUniformBuffers.get(),
			performanceMonitor.vk.numDescriptorStorageBuffers.get()
		).c_str());
	}
	vkObjDS->descriptorSet = result;

	sint32 textureCount = shader->resourceMapping.getTextureCount();

	std::vector<VkWriteDescriptorSet> descriptorWrites;
	std::vector<VkDescriptorImageInfo> textureArray;
	for (int i = 0; i < textureCount; ++i)
	{
		VkDescriptorImageInfo info{};
		const auto relative_textureUnit = shader->resourceMapping.getTextureUnitFromBindingPoint(i);
		auto hostTextureUnit = relative_textureUnit;
		auto textureDim = shader->textureUnitDim[relative_textureUnit];
		auto texUnitRegIndex = hostTextureUnit * 7;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
			break;
		case LatteConst::ShaderType::Pixel:
			hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
			break;
		case LatteConst::ShaderType::Geometry:
			hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
			texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
			break;
		default:
			UNREACHABLE;
		}

		auto textureView = m_state.boundTexture[hostTextureUnit];
		if (!textureView)
		{
			if (textureDim == Latte::E_DIM::DIM_1D)
			{
				info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
				info.imageView = nullTexture1D.view;
				info.sampler = nullTexture1D.sampler;
				textureArray.emplace_back(info);
			}
			else
			{
				info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
				info.imageView = nullTexture2D.view;
				info.sampler = nullTexture2D.sampler;
				textureArray.emplace_back(info);
			}
			continue;
		}
		// safeguard to avoid using mismatching texture dimensions
		// if we properly support aux hash for vs/gs this should never trigger
		if (textureDim == Latte::E_DIM::DIM_1D && (textureView->dim != Latte::E_DIM::DIM_1D))
		{
			// should be 1D
			info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			info.imageView = nullTexture1D.view;
			info.sampler = nullTexture1D.sampler;
			textureArray.emplace_back(info);
			// cemuLog_log(LogType::Force, "Vulkan-Info: Shader 0x{:016x} uses 1D texture but bound texture has mismatching type (dim: 0x{:02x})", shader->baseHash, textureView->gx2Dim);
			continue;
		}
		else if (textureDim == Latte::E_DIM::DIM_2D && (textureView->dim != Latte::E_DIM::DIM_2D && textureView->dim != Latte::E_DIM::DIM_2D_MSAA))
		{
			// should be 2D
			// is GPU7 fine with 2D access to a 2D_ARRAY texture?
			info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			info.imageView = nullTexture2D.view;
			info.sampler = nullTexture2D.sampler;
			textureArray.emplace_back(info);
			// cemuLog_log(LogType::Force, "Vulkan-Info: Shader 0x{:016x} uses 2D texture but bound texture has mismatching type (dim: 0x{:02x})", shader->baseHash, textureView->gx2Dim);
			continue;
		}

		info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkSamplerCustomBorderColorCreateInfoEXT samplerCustomBorderColor{};

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

		LatteTexture* baseTexture = textureView->baseTexture;
		// get texture register word 0
		uint32 word4 = LatteGPUState.contextRegister[texUnitRegIndex + 4];
		const uint32 correctedWord4 = MarioKart8LobbyChannels::AdjustShadowSwizzle(
			CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(),
			shader->baseHash, shader->shaderType == LatteConst::ShaderType::Pixel, relative_textureUnit,
			(uint32)textureView->format, baseTexture->width, baseTexture->height, baseTexture->depth,
			textureView->numMip, textureView->numSlice, textureView->dim == Latte::E_DIM::DIM_2D, word4);
		if (correctedWord4 != word4)
			cemuLog_logOnce(LogType::Force, "Mario Kart 8: correcting lobby shadow-mask channel routing");
		word4 = correctedWord4;
		auto imageViewObj = textureView->GetSamplerView(word4);
		info.imageView = imageViewObj->m_textureImageView;
		vkObjDS->addRef(imageViewObj);

		// track relation between view and descriptor set
		vectorAppendUnique(dsInfo->list_referencedViews, textureView);
		textureView->AddDescriptorSetReference(dsInfo);

		if (!baseTexture->IsCompressedFormat())
			vectorAppendUnique(dsInfo->list_fboCandidates, (LatteTextureVk*)baseTexture);

		uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relative_textureUnit];
		if (stageSamplerIndex != LATTE_DECOMPILER_SAMPLER_NONE)
		{
			uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
			const _LatteRegisterSetSampler* samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;

			// lod
			uint32 iMinLOD = samplerWords->WORD1.get_MIN_LOD();
			uint32 iMaxLOD = samplerWords->WORD1.get_MAX_LOD();
			sint32 iLodBias = samplerWords->WORD1.get_LOD_BIAS();

			// apply relative lod bias from graphic pack
			if (baseTexture->overwriteInfo.hasRelativeLodBias)
				iLodBias += baseTexture->overwriteInfo.relativeLodBias;
			// apply absolute lod bias from graphic pack
			if (baseTexture->overwriteInfo.hasLodBias)
				iLodBias = baseTexture->overwriteInfo.lodBias;

			auto filterMip = samplerWords->WORD0.get_MIP_FILTER();
			if (filterMip == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::NONE)
			{
				samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
				samplerInfo.minLod = 0;
				samplerInfo.maxLod = 0.25f;
			}
			else if (filterMip == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::POINT)
			{
				samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
				samplerInfo.minLod = (float)iMinLOD / 64.0f;
				samplerInfo.maxLod = (float)iMaxLOD / 64.0f;
			}
			else if (filterMip == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::LINEAR)
			{
				samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
				samplerInfo.minLod = (float)iMinLOD / 64.0f;
				samplerInfo.maxLod = (float)iMaxLOD / 64.0f;
			}
			else
			{
				// fallback for invalid constants
				samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
				samplerInfo.minLod = (float)iMinLOD / 64.0f;
				samplerInfo.maxLod = (float)iMaxLOD / 64.0f;
			}

			auto filterMin = samplerWords->WORD0.get_XY_MIN_FILTER();
			cemu_assert_debug(filterMin != Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::BICUBIC); // todo
			samplerInfo.minFilter = (filterMin == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT || filterMin == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

			auto filterMag = samplerWords->WORD0.get_XY_MAG_FILTER();
			samplerInfo.magFilter = (filterMag == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT || filterMin == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

			auto filterZ = samplerWords->WORD0.get_Z_FILTER();
			// todo: z-filter for texture array samplers is customizable for GPU7 but OpenGL/Vulkan doesn't expose this functionality?

			static const VkSamplerAddressMode s_vkClampTable[] = {
				VK_SAMPLER_ADDRESS_MODE_REPEAT, // WRAP
				VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // MIRROR
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // CLAMP_LAST_TEXEL
				VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, // MIRROR_ONCE_LAST_TEXEL
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // unsupported HALF_BORDER
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, // unsupported MIRROR_ONCE_HALF_BORDER
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, // CLAMP_BORDER
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER // MIRROR_ONCE_BORDER
			};

			auto clampX = samplerWords->WORD0.get_CLAMP_X();
			auto clampY = samplerWords->WORD0.get_CLAMP_Y();
			auto clampZ = samplerWords->WORD0.get_CLAMP_Z();

			samplerInfo.addressModeU = s_vkClampTable[(size_t)clampX];
			samplerInfo.addressModeV = s_vkClampTable[(size_t)clampY];
			samplerInfo.addressModeW = s_vkClampTable[(size_t)clampZ];

			auto maxAniso = samplerWords->WORD0.get_MAX_ANISO_RATIO();

			if (baseTexture->overwriteInfo.anisotropicLevel >= 0)
				maxAniso = baseTexture->overwriteInfo.anisotropicLevel;

			if (maxAniso > 0)
			{
				samplerInfo.anisotropyEnable = VK_TRUE;
				samplerInfo.maxAnisotropy = (float)(1 << maxAniso);
			}
			else
			{
				samplerInfo.anisotropyEnable = VK_FALSE;
				samplerInfo.maxAnisotropy = 1.0f;
			}

			samplerInfo.mipLodBias = (float)iLodBias / 64.0f;
			if (IsMN9BrokenBoxSample(shader, relative_textureUnit, baseTexture))
			{
				samplerInfo.minLod = 0.0f;
				samplerInfo.maxLod = 0.0f;
				samplerInfo.mipLodBias = 0.0f;
				cemuLog_logOnce(LogType::Force,
					"Mighty No. 9: forcing mip 0 for the corrupt box atlas material (Vulkan)");
			}
			// depth compare
			uint8 depthCompareMode = shader->textureUsesDepthCompare[relative_textureUnit] ? 1 : 0;
			static const VkCompareOp s_vkCompareOps[]
			{
				VK_COMPARE_OP_NEVER,
				VK_COMPARE_OP_LESS,
				VK_COMPARE_OP_EQUAL,
				VK_COMPARE_OP_LESS_OR_EQUAL,
				VK_COMPARE_OP_GREATER,
				VK_COMPARE_OP_NOT_EQUAL,
				VK_COMPARE_OP_GREATER_OR_EQUAL,
				VK_COMPARE_OP_ALWAYS,
			};
			if (depthCompareMode == 1)
			{
				samplerInfo.compareEnable = VK_TRUE;
				samplerInfo.compareOp = s_vkCompareOps[(size_t)samplerWords->WORD0.get_DEPTH_COMPARE_FUNCTION()];
			}
			else
			{
				samplerInfo.compareEnable = VK_FALSE;
			}

			// border
			auto borderType = samplerWords->WORD0.get_BORDER_COLOR_TYPE();

			if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::TRANSPARENT_BLACK)
				samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			else if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::OPAQUE_BLACK)
				samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
			else if (borderType == Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE::OPAQUE_WHITE)
				samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			else
			{
				if (this->m_featureControl.deviceExtensions.custom_border_color_without_format)
				{
					samplerCustomBorderColor.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
					samplerCustomBorderColor.format = VK_FORMAT_UNDEFINED;

					_LatteRegisterSetSamplerBorderColor* borderColorReg;
					if (shader->shaderType == LatteConst::ShaderType::Vertex)
						borderColorReg = LatteGPUState.contextNew.TD_VS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
					else if (shader->shaderType == LatteConst::ShaderType::Pixel)
						borderColorReg = LatteGPUState.contextNew.TD_PS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
					else // geometry
						borderColorReg = LatteGPUState.contextNew.TD_GS_SAMPLER_BORDER_COLOR + stageSamplerIndex;
					samplerCustomBorderColor.customBorderColor.float32[0] = borderColorReg->red.get_channelValue();
					samplerCustomBorderColor.customBorderColor.float32[1] = borderColorReg->green.get_channelValue();
					samplerCustomBorderColor.customBorderColor.float32[2] = borderColorReg->blue.get_channelValue();
					samplerCustomBorderColor.customBorderColor.float32[3] = borderColorReg->alpha.get_channelValue();

					samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
					samplerInfo.pNext = &samplerCustomBorderColor;
				}
				else
				{
					// default to transparent black if custom border color is not supported
					samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
				}
			}
		}

		VKRObjectSampler* samplerObj = VKRObjectSampler::GetOrCreateSampler(&samplerInfo);
		vkObjDS->addRef(samplerObj);
		info.sampler = samplerObj->GetSampler();
		textureArray.emplace_back(info);
	}

	if (textureCount > 0)
	{
		for (sint32 i = 0; i < textureCount; i++)
		{
			VkWriteDescriptorSet write_descriptor{};
			write_descriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write_descriptor.dstSet = result;
			write_descriptor.dstBinding = shader->resourceMapping.getTextureBaseBindingPoint() + i;
			write_descriptor.dstArrayElement = 0;
			write_descriptor.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write_descriptor.descriptorCount = 1;
			write_descriptor.pImageInfo = textureArray.data() + i;
			descriptorWrites.emplace_back(write_descriptor);
			performanceMonitor.vk.numDescriptorSamplerTextures.increment();
			dsInfo->statsNumSamplerTextures++;
		}
	}

	// descriptor for uniform vars (as buffer)

	VkDescriptorBufferInfo uniformVarsBufferInfo{};

	if (shader->resourceMapping.uniformVarsBufferBindingPoint >= 0)
	{
		uniformVarsBufferInfo.buffer = m_uniformVarBuffer;
		uniformVarsBufferInfo.offset = 0; // fixed offset is always zero since we only use dynamic offsets
		uniformVarsBufferInfo.range = shader->uniform.uniformRangeSize;

		VkWriteDescriptorSet write_descriptor{};
		write_descriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write_descriptor.dstSet = result;
		write_descriptor.dstBinding = shader->resourceMapping.uniformVarsBufferBindingPoint;
		write_descriptor.dstArrayElement = 0;
		write_descriptor.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		write_descriptor.descriptorCount = 1;
		write_descriptor.pBufferInfo = &uniformVarsBufferInfo;
		descriptorWrites.emplace_back(write_descriptor);
		performanceMonitor.vk.numDescriptorDynUniformBuffers.increment();
		dsInfo->statsNumDynUniformBuffers++;
	}

	// descriptor for uniform buffers

	VkDescriptorBufferInfo uniformBufferInfo{};
	uniformBufferInfo.buffer = m_useHostMemoryForCache ? m_importedMem : m_bufferCache;
	uniformBufferInfo.offset = 0; // fixed offset is always zero since we only use dynamic offsets

	if (m_vendor == GfxVendor::AMD)
	{
		// on AMD we enable robust buffer access and map the remaining range of the buffer
		uniformBufferInfo.range = VK_WHOLE_SIZE;
	}
	else
	{
		// on other vendors (which may not allow large range values) we disable robust buffer access and use a fixed size
		// update: starting with their Vulkan 1.2 drivers Nvidia now also prevents out-of-bounds access. Unlike on AMD, we can't use VK_WHOLE_SIZE due to 64KB size limit of uniforms
		// as a workaround we set the size to the allowed maximum. A proper solution would be to use SSBOs for large uniforms / uniforms with unknown size?
		uniformBufferInfo.range = 1024 * 16 * 4; // XCX
	}

	for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
	{
		if (shader->resourceMapping.uniformBuffersBindingPoint[i] >= 0)
		{
			VkWriteDescriptorSet write_descriptor{};
			write_descriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write_descriptor.dstSet = result;
			write_descriptor.dstBinding = shader->resourceMapping.uniformBuffersBindingPoint[i];
			write_descriptor.dstArrayElement = 0;
			write_descriptor.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			write_descriptor.descriptorCount = 1;
			write_descriptor.pBufferInfo = &uniformBufferInfo;
			descriptorWrites.emplace_back(write_descriptor);
			performanceMonitor.vk.numDescriptorDynUniformBuffers.increment();
			dsInfo->statsNumDynUniformBuffers++;
		}
	}


	VkDescriptorBufferInfo tfStorageBufferInfo{};

	if (shader->resourceMapping.tfStorageBindingPoint >= 0)
	{
		tfStorageBufferInfo.buffer = m_xfbRingBuffer;
		tfStorageBufferInfo.offset = 0; // offset is calculated in shader
		tfStorageBufferInfo.range = VK_WHOLE_SIZE;

		VkWriteDescriptorSet write_descriptor{};
		write_descriptor.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write_descriptor.dstSet = result;
		write_descriptor.dstBinding = shader->resourceMapping.tfStorageBindingPoint;
		write_descriptor.dstArrayElement = 0;
		write_descriptor.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write_descriptor.descriptorCount = 1;
		write_descriptor.pBufferInfo = &tfStorageBufferInfo;
		descriptorWrites.emplace_back(write_descriptor);
		performanceMonitor.vk.numDescriptorStorageBuffers.increment();
		dsInfo->statsNumStorageBuffers++;
	}

	if (!descriptorWrites.empty())
		vkUpdateDescriptorSets(m_logicalDevice, (uint32)descriptorWrites.size(), descriptorWrites.data(), 0, nullptr);

	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
	{
		pipeline_info->vertex_ds_cache[stateHash] = dsInfo;
		break;
	}
	case LatteConst::ShaderType::Pixel:
	{
		pipeline_info->pixel_ds_cache[stateHash] = dsInfo;
		break;
	}
	case LatteConst::ShaderType::Geometry:
	{
		pipeline_info->geometry_ds_cache[stateHash] = dsInfo;
		break;
	}
	default:
		UNREACHABLE;
	}

	return dsInfo;
}

void VulkanRenderer::sync_inputTexturesChanged()
{
	bool writeFlushRequired = false;

	if (m_state.activeVertexDS)
	{
		for (auto& tex : m_state.activeVertexDS->list_fboCandidates)
		{
			tex->m_vkFlushIndex_read = m_state.currentFlushIndex;
			if (tex->m_vkFlushIndex_write == m_state.currentFlushIndex)
				writeFlushRequired = true;
		}
	}
	if (m_state.activeGeometryDS)
	{
		for (auto& tex : m_state.activeGeometryDS->list_fboCandidates)
		{
			tex->m_vkFlushIndex_read = m_state.currentFlushIndex;
			if (tex->m_vkFlushIndex_write == m_state.currentFlushIndex)
				writeFlushRequired = true;
		}
	}
	if (m_state.activePixelDS)
	{
		for (auto& tex : m_state.activePixelDS->list_fboCandidates)
		{
			tex->m_vkFlushIndex_read = m_state.currentFlushIndex;
			if (tex->m_vkFlushIndex_write == m_state.currentFlushIndex)
				writeFlushRequired = true;
		}
	}
	// barrier here
	if (writeFlushRequired)
	{
		VkMemoryBarrier memoryBarrier{};
		memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memoryBarrier.srcAccessMask = 0;
		memoryBarrier.dstAccessMask = 0;

		VkPipelineStageFlags srcStage = 0;
		VkPipelineStageFlags dstStage = 0;

		// src
		srcStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		memoryBarrier.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		srcStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		memoryBarrier.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		// dst
		dstStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		memoryBarrier.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

		dstStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		memoryBarrier.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(m_state.currentCommandBuffer, srcStage, dstStage, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);

		performanceMonitor.vk.numDrawBarriersPerFrame.increment();

		m_state.currentFlushIndex++;
	}
}

void VulkanRenderer::sync_RenderPassLoadTextures(CachedFBOVk* fboVk)
{
	bool readFlushRequired = false;
	// always called after draw_inputTexturesChanged()
	for (auto& tex : fboVk->GetTextures())
	{
		LatteTextureVk* texVk = (LatteTextureVk*)tex;
		// write-before-write
		if (texVk->m_vkFlushIndex_write == m_state.currentFlushIndex)
			readFlushRequired = true;


		texVk->m_vkFlushIndex_write = m_state.currentFlushIndex;
		// todo - also check for write-before-write ?
		if (texVk->m_vkFlushIndex_read == m_state.currentFlushIndex)
			readFlushRequired = true;
	}
	// barrier here
	if (readFlushRequired)
	{
		VkMemoryBarrier memoryBarrier{};
		memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memoryBarrier.srcAccessMask = 0;
		memoryBarrier.dstAccessMask = 0;

		VkPipelineStageFlags srcStage = 0;
		VkPipelineStageFlags dstStage = 0;

		// src
		srcStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		memoryBarrier.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		srcStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		memoryBarrier.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		// dst
		dstStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		memoryBarrier.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

		dstStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		memoryBarrier.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

		vkCmdPipelineBarrier(m_state.currentCommandBuffer, srcStage, dstStage, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);

		performanceMonitor.vk.numDrawBarriersPerFrame.increment();

		m_state.currentFlushIndex++;
	}
}

void VulkanRenderer::sync_RenderPassStoreTextures(CachedFBOVk* fboVk)
{
	uint32 flushIndex = m_state.currentFlushIndex;
	for (auto& tex : fboVk->GetTextures())
	{
		LatteTextureVk* texVk = (LatteTextureVk*)tex;
		texVk->m_vkFlushIndex_write = flushIndex;
	}
}

void VulkanRenderer::draw_prepareDescriptorSets(PipelineInfo* pipeline_info, VkDescriptorSetInfo*& vertexDS, VkDescriptorSetInfo*& pixelDS, VkDescriptorSetInfo*& geometryDS)
{
	const auto vertexShader = LatteSHRC_GetActiveVertexShader();
	const auto geometryShader = LatteSHRC_GetActiveGeometryShader();
	const auto pixelShader = LatteSHRC_GetActivePixelShader();

	auto prepareShaderDescriptors = [this, &pipeline_info](LatteDecompilerShader* shader) -> VkDescriptorSetInfo* {
		if (!shader)
			return nullptr;
		auto descriptorSetInfo = draw_getOrCreateDescriptorSet(pipeline_info, shader);
		descriptorSetInfo->m_vkObjDescriptorSet->flagForCurrentCommandBuffer();
		return descriptorSetInfo;
	};

	vertexDS = prepareShaderDescriptors(vertexShader);
	pixelDS = prepareShaderDescriptors(pixelShader);
	geometryDS = prepareShaderDescriptors(geometryShader);
}

void VulkanRenderer::draw_updateVkBlendConstants()
{
	uint32* blendColorConstant = LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED;
	vkCmdSetBlendConstants(m_state.currentCommandBuffer, (const float*)blendColorConstant);
}

void VulkanRenderer::draw_updateDepthBias(bool forceUpdate)
{
	uint32 frontScaleU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_SCALE.getRawValue();
	uint32 frontOffsetU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_OFFSET.getRawValue();
	uint32 offsetClampU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_CLAMP.getRawValue();

	if (forceUpdate == false &&
		m_state.prevPolygonFrontScaleU32 == frontScaleU32 &&
		m_state.prevPolygonFrontOffsetU32 == frontOffsetU32 &&
		m_state.prevPolygonFrontClampU32 == offsetClampU32)
		return;

	m_state.prevPolygonFrontScaleU32 = frontScaleU32;
	m_state.prevPolygonFrontOffsetU32 = frontOffsetU32;
	m_state.prevPolygonFrontClampU32 = offsetClampU32;

	float frontScale = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE();
	float frontOffset = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
	float offsetClamp = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();

	frontScale /= 16.0f;

	vkCmdSetDepthBias(m_state.currentCommandBuffer, frontOffset, offsetClamp, frontScale);
}

bool s_syncOnNextDraw = false;

void VulkanRenderer::draw_setRenderPass()
{
	CachedFBOVk* fboVk = m_state.activeFBO;

	// update self-dependency flag
	if (m_state.descriptorSetsChanged || m_state.activeRenderpassFBO != fboVk)
	{
		m_state.hasRenderSelfDependency = fboVk->CheckForCollision(m_state.activeVertexDS, m_state.activeGeometryDS, m_state.activePixelDS);
	}

	auto vkObjRenderPass = fboVk->GetRenderPassObj();
	auto vkObjFramebuffer = fboVk->GetFramebufferObj();

	bool overridePassReuse = m_state.hasRenderSelfDependency && (GetConfig().vk_accurate_barriers || m_state.activePipelineInfo->neverSkipAccurateBarrier);
	const bool forceDescriptorBoundary = ActiveSettings::ApplyAssassinsCreed3VulkanWorkarounds() && m_state.descriptorSetsChanged;

	if (!overridePassReuse && !forceDescriptorBoundary && m_state.activeRenderpassFBO == fboVk)
	{
		if (m_state.descriptorSetsChanged)
			sync_inputTexturesChanged();
		return;
	}
	draw_endRenderPass();
	if (m_state.descriptorSetsChanged)
		sync_inputTexturesChanged();

	// assume that FBO changed, update self-dependency state
	m_state.hasRenderSelfDependency = fboVk->CheckForCollision(m_state.activeVertexDS, m_state.activeGeometryDS, m_state.activePixelDS);

	sync_RenderPassLoadTextures(fboVk);

	if (m_featureControl.deviceExtensions.dynamic_rendering)
	{
		vkCmdBeginRenderingKHR(m_state.currentCommandBuffer, fboVk->GetRenderingInfo());
	}
	else
	{
		VkRenderPass renderPass = vkObjRenderPass->m_renderPass;
		VkFramebuffer framebuffer = vkObjFramebuffer->m_frameBuffer;
		VkExtent2D extend = fboVk->GetExtend();

		VkRenderPassBeginInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassInfo.renderPass = renderPass;
		renderPassInfo.framebuffer = framebuffer;
		renderPassInfo.renderArea.offset = { 0, 0 };
		renderPassInfo.renderArea.extent = extend;
		renderPassInfo.clearValueCount = 0;
		vkCmdBeginRenderPass(m_state.currentCommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	}

	m_state.activeRenderpassFBO = fboVk;

	vkObjRenderPass->flagForCurrentCommandBuffer();
	vkObjFramebuffer->flagForCurrentCommandBuffer();

	performanceMonitor.vk.numBeginRenderpassPerFrame.increment();
}

void VulkanRenderer::draw_endRenderPass()
{
	if (!m_state.activeRenderpassFBO)
		return;
	if (m_featureControl.deviceExtensions.dynamic_rendering)
		vkCmdEndRenderingKHR(m_state.currentCommandBuffer);
	else
		vkCmdEndRenderPass(m_state.currentCommandBuffer);
	sync_RenderPassStoreTextures(m_state.activeRenderpassFBO);
	m_state.activeRenderpassFBO = nullptr;
}

// Defined in the Common renderer
void LatteDraw_handleSpecialState8_clearAsDepth();

// transfer depth buffer data to color buffer
void VulkanRenderer::draw_handleSpecialState5()
{
	LatteMRT::UpdateCurrentFBO();
	LatteRenderTarget_updateViewport();

	LatteTextureView* colorBuffer = LatteMRT::GetColorAttachment(0);
	LatteTextureView* depthBuffer = LatteMRT::GetDepthAttachment();

	sint32 vpWidth, vpHeight;
	LatteMRT::GetVirtualViewportDimensions(vpWidth, vpHeight);

	surfaceCopy_copySurfaceWithFormatConversion(
		depthBuffer->baseTexture, depthBuffer->firstMip, depthBuffer->firstSlice,
		colorBuffer->baseTexture, colorBuffer->firstMip, colorBuffer->firstSlice,
		vpWidth, vpHeight);
}

void VulkanRenderer::draw_beginSequence()
{
	if (RevelationsPipelinePreparation::RuntimeEnabled() && !s_preparationTestDrained &&
		std::getenv("CEMU_RE_VK_TEST_DRAIN") && LatteGPUState.frameCounter >= 500)
	{
		RevelationsClearPreparedPipelines();
		s_preparationTestDrained = true;
		cemuLog_log(LogType::Force, "[RE:VKPREP] event=test_drain_complete");
	}
	const bool traceRE = RevelationsVulkanTiming::RuntimeEnabled();
	RevelationsVulkanTiming::Scope shaderTimer(RevelationsVulkanTiming::Stage::Shader, traceRE);
	m_state.drawSequenceSkip = false;

	bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;

	// update shader state
	LatteSHRC_UpdateActiveShaders();
	shaderTimer.Stop();
	if (LatteGPUState.activeShaderHasError)
	{
		cemuLog_logDebugOnce(LogType::Force, "Skipping drawcalls due to shader error");
		m_state.drawSequenceSkip = true;
		cemu_assert_debug(false);
		return;
	}

	// update render target and texture state
	RevelationsVulkanTiming::Scope textureTimer(RevelationsVulkanTiming::Stage::Textures, traceRE);
	LatteGPUState.requiresTextureBarrier = false;
	while (true)
	{
		LatteGPUState.repeatTextureInitialization = false;
		if (!LatteMRT::UpdateCurrentFBO())
		{
			debug_printf("Rendertarget invalid\n");
			m_state.drawSequenceSkip = true;
			return; // no render target
		}

		if (!hasValidFramebufferAttached && !streamoutEnable)
		{
			debug_printf("Drawcall with no color buffer or depth buffer attached\n");
			m_state.drawSequenceSkip = true;
			return; // no render target
		}
		LatteTexture_updateTextures();
		if (!LatteGPUState.repeatTextureInitialization)
			break;
	}

	// apply render target
	LatteMRT::ApplyCurrentState();

	// viewport and scissor box
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();

	// check for conditions which would turn the drawcalls into no-ops
	bool rasterizerEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() == false;

	// GX2SetSpecialState(0, true) enables DX_RASTERIZATION_KILL, but still expects depth writes to happen? -> Research which stages are disabled by DX_RASTERIZATION_KILL exactly
	// for now we use a workaround:
	if (!LatteGPUState.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
		rasterizerEnable = true;

	if (rasterizerEnable == false && streamoutEnable == false)
		m_state.drawSequenceSkip = true;
}

void VulkanRenderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, bool isFirst)
{
	const bool traceRE = RevelationsVulkanTiming::RuntimeEnabled();
	if (m_state.drawSequenceSkip)
	{
		if (traceRE) ++RevelationsVulkanTiming::frame.skippedSequences;
		LatteGPUState.drawCallCounter++;
		return;
	}

	// fast clear color as depth
	if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
	{
		LatteDraw_handleSpecialState8_clearAsDepth();
		LatteGPUState.drawCallCounter++;
		return;
	}
	else if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
	{
		draw_handleSpecialState5();
		LatteGPUState.drawCallCounter++;
		return;
	}

	// prepare streamout
	RevelationsVulkanTiming::Scope bufferTimer(RevelationsVulkanTiming::Stage::Buffers, traceRE);
	m_streamoutState.verticesPerInstance = count;
	// SSBO streamout is packed per draw, unlike the absolute input vertex index.
	m_streamoutState.vertexIndexBase = ActiveSettings::UseInjusticeLoopConstants() &&
		indexType == LatteIndexType::AUTO &&
		static_cast<LattePrimitiveMode>(LatteGPUState.contextRegister[mmVGT_PRIMITIVE_TYPE]) == LattePrimitiveMode::POINTS
		? baseVertex + count * baseInstance : 0;
	LatteStreamout_PrepareDrawcall(count, instanceCount);

	// update uniform vars
	LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();

	if (vertexShader)
		uniformData_updateUniformVars(VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX, vertexShader);
	if (pixelShader)
		uniformData_updateUniformVars(VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT, pixelShader);
	if (geometryShader)
		uniformData_updateUniformVars(VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY, geometryShader);
	// store where the read pointer should go after command buffer execution
	m_cmdBufferUniformRingbufIndices[m_commandBufferIndex] = m_uniformVarBufferWriteIndex;

	// process index data
	const LattePrimitiveMode primitiveMode = static_cast<LattePrimitiveMode>(LatteGPUState.contextRegister[mmVGT_PRIMITIVE_TYPE]);

	Renderer::INDEX_TYPE hostIndexType;
	uint32 hostIndexCount;
	uint32 indexMin = 0;
	uint32 indexMax = 0;
	Renderer::IndexAllocation indexAllocation;
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMin, indexMax, hostIndexType, hostIndexCount, indexAllocation);
	VKRSynchronizedHeapAllocator::AllocatorReservation* indexReservation = (VKRSynchronizedHeapAllocator::AllocatorReservation*)indexAllocation.rendererInternal;
	// update index binding
	bool isPrevIndexData = false;
	if (hostIndexType != INDEX_TYPE::NONE)
	{
		uint32 indexBufferIndex = indexReservation->bufferIndex;
		uint32 indexBufferOffset = indexReservation->bufferOffset;
		if (m_state.activeIndexBufferOffset != indexBufferOffset || m_state.activeIndexBufferIndex != indexBufferIndex || m_state.activeIndexType != hostIndexType)
		{
			m_state.activeIndexType = hostIndexType;
			m_state.activeIndexBufferOffset = indexBufferOffset;
			m_state.activeIndexBufferIndex = indexBufferIndex;
			VkIndexType vkType;
			if (hostIndexType == INDEX_TYPE::U16)
				vkType = VK_INDEX_TYPE_UINT16;
			else if (hostIndexType == INDEX_TYPE::U32)
				vkType = VK_INDEX_TYPE_UINT32;
			else
				cemu_assert(false);
			vkCmdBindIndexBuffer(m_state.currentCommandBuffer, indexReservation->vkBuffer, indexBufferOffset, vkType);
		}
		else
			isPrevIndexData = true;
	}

	if (m_useHostMemoryForCache)
	{
		// direct memory access (Wii U memory space imported as a Vulkan buffer), update buffer bindings
		draw_updateVertexBuffersDirectAccess();
		LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
		if (vertexShader)
			draw_updateUniformBuffersDirectAccess(vertexShader, mmSQ_VTX_UNIFORM_BLOCK_START, LatteConst::ShaderType::Vertex);
		LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
		if (geometryShader)
			draw_updateUniformBuffersDirectAccess(geometryShader, mmSQ_GS_UNIFORM_BLOCK_START, LatteConst::ShaderType::Geometry);
		LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
		if (pixelShader)
			draw_updateUniformBuffersDirectAccess(pixelShader, mmSQ_PS_UNIFORM_BLOCK_START, LatteConst::ShaderType::Pixel);
	}
	else
	{
		// synchronize vertex and uniform cache and update buffer bindings
		LatteBufferCache_Sync(indexMin + baseVertex, indexMax + baseVertex, baseInstance, instanceCount);
	}

	PipelineInfo* pipeline_info;
	bufferTimer.Stop();
	RevelationsVulkanTiming::Scope pipelineTimer(RevelationsVulkanTiming::Stage::Pipeline, traceRE);

	if (!isFirst)
	{
		if (m_state.activePipelineInfo->minimalStateHash != draw_calculateMinimalGraphicsPipelineHash(vertexShader->compatibleFetchShader, LatteGPUState.contextNew))
		{
			// pipeline changed
			pipeline_info = draw_getOrCreateGraphicsPipeline(count);
			m_state.activePipelineInfo = pipeline_info;
		}
		else
		{
			pipeline_info = m_state.activePipelineInfo;
#ifdef CEMU_DEBUG_ASSERT
			auto pipeline_info2 = draw_getOrCreateGraphicsPipeline(count);
			if (pipeline_info != pipeline_info2)
			{
				cemu_assert_debug(false);
			}
#endif
		}
	}
	else
	{
		pipeline_info = draw_getOrCreateGraphicsPipeline(count);
		m_state.activePipelineInfo = pipeline_info;
	}

	auto vkObjPipeline = pipeline_info->m_vkrObjPipeline;
	pipelineTimer.Stop();
	if (vkObjPipeline->GetPipeline() == VK_NULL_HANDLE)
	{
		if (traceRE) ++RevelationsVulkanTiming::frame.nullPipelines;
		// invalid/uninitialized pipeline
		m_state.activeVertexDS = nullptr;
		return;
	}


	RevelationsVulkanTiming::Scope descriptorTimer(RevelationsVulkanTiming::Stage::Descriptors, traceRE);
	VkDescriptorSetInfo *vertexDS = nullptr, *pixelDS = nullptr, *geometryDS = nullptr;
	if (!isFirst && m_state.activeVertexDS)
	{
		vertexDS = m_state.activeVertexDS;
		pixelDS = m_state.activePixelDS;
		geometryDS = m_state.activeGeometryDS;
		m_state.descriptorSetsChanged = false;
	}
	else
	{
		draw_prepareDescriptorSets(pipeline_info, vertexDS, pixelDS, geometryDS);
		m_state.activeVertexDS = vertexDS;
		m_state.activePixelDS = pixelDS;
		m_state.activeGeometryDS = geometryDS;
		m_state.descriptorSetsChanged = true;
	}

	draw_setRenderPass();

	if (m_state.currentPipeline != vkObjPipeline->GetPipeline())
	{
		vkCmdBindPipeline(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vkObjPipeline->GetPipeline());
		vkObjPipeline->flagForCurrentCommandBuffer();
		m_state.currentPipeline = vkObjPipeline->GetPipeline();
		// depth bias
		if (pipeline_info->usesDepthBias)
			draw_updateDepthBias(true);
	}
	else
	{
		if (pipeline_info->usesDepthBias)
			draw_updateDepthBias(false);
	}

	// update blend constants
	if (pipeline_info->usesBlendConstants)
		draw_updateVkBlendConstants();

	// update descriptor sets
	uint32_t dynamicOffsets[17 * 2];
	if (vertexDS && pixelDS)
	{
		// update vertex and pixel descriptor set in a single call to vkCmdBindDescriptorSets
		sint32 numDynOffsetsVS;
		sint32 numDynOffsetsPS;
		draw_prepareDynamicOffsetsForDescriptorSet(VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX, dynamicOffsets, numDynOffsetsVS,
			pipeline_info);
		draw_prepareDynamicOffsetsForDescriptorSet(VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT, dynamicOffsets + numDynOffsetsVS, numDynOffsetsPS,
			pipeline_info);

		VkDescriptorSet dsArray[2];
		dsArray[0] = vertexDS->m_vkObjDescriptorSet->descriptorSet;
		dsArray[1] = pixelDS->m_vkObjDescriptorSet->descriptorSet;

		vkCmdBindDescriptorSets(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			vkObjPipeline->m_pipelineLayout, 0, 2, dsArray, numDynOffsetsVS + numDynOffsetsPS,
			dynamicOffsets);
	}
	else if (vertexDS)
	{
		sint32 numDynOffsets;
		draw_prepareDynamicOffsetsForDescriptorSet(VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX, dynamicOffsets, numDynOffsets,
			pipeline_info);
		vkCmdBindDescriptorSets(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			vkObjPipeline->m_pipelineLayout, 0, 1, &vertexDS->m_vkObjDescriptorSet->descriptorSet, numDynOffsets,
			dynamicOffsets);
	}
	else if (pixelDS)
	{
		sint32 numDynOffsets;
		draw_prepareDynamicOffsetsForDescriptorSet(VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT, dynamicOffsets, numDynOffsets,
			pipeline_info);
		vkCmdBindDescriptorSets(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			vkObjPipeline->m_pipelineLayout, 1, 1, &pixelDS->m_vkObjDescriptorSet->descriptorSet, numDynOffsets,
			dynamicOffsets);
	}
	if (geometryDS)
	{
		sint32 numDynOffsets;
		draw_prepareDynamicOffsetsForDescriptorSet(VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY, dynamicOffsets, numDynOffsets,
			pipeline_info);
		vkCmdBindDescriptorSets(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			vkObjPipeline->m_pipelineLayout, 2, 1, &geometryDS->m_vkObjDescriptorSet->descriptorSet, numDynOffsets,
			dynamicOffsets);
	}

	auto activeRenderpassFbo = m_state.activeRenderpassFBO;
	descriptorTimer.Stop();
	RevelationsVulkanTiming::Scope submitTimer(RevelationsVulkanTiming::Stage::Submit, traceRE);

	// draw
	if (const auto traceSample = GetCompatibilityDrawTraceSample())
	{
		std::string textures;
		if (pixelShader)
		{
			const sint32 textureCount = pixelShader->resourceMapping.getTextureCount();
			for (sint32 index = 0; index < textureCount; index++)
			{
				const sint32 unit = pixelShader->resourceMapping.getTextureUnitFromBindingPoint(index);
				auto* view = m_state.boundTexture[LATTE_CEMU_PS_TEX_UNIT_BASE + unit];
				if (!view || !view->baseTexture)
					continue;
				const LatteTexture* texture = view->baseTexture;
				const sint32 resourceRegister = Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + unit * 7;
				const uint32 word4 = LatteGPUState.contextRegister[resourceRegister + 4];
				const uint32 samplerAssignment = pixelShader->textureUnitSamplerAssignment[unit];
				uint32 samplerWord0 = 0;
				uint32 samplerWord1 = 0;
				if (samplerAssignment != LATTE_DECOMPILER_SAMPLER_NONE)
				{
					const uint32 samplerIndex = samplerAssignment + LatteDecompiler_getTextureSamplerBaseIndex(pixelShader->shaderType);
					const _LatteRegisterSetSampler* samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;
					samplerWord0 = samplerWords->WORD0.getRawValue();
					samplerWord1 = samplerWords->WORD1.getRawValue();
				}
				VkImageSubresource subresource{};
				subresource.mipLevel = view->firstMip;
				subresource.arrayLayer = view->firstSlice;
				const auto layout = static_cast<LatteTextureVk*>(view->baseTexture)->GetImageLayout(subresource);
				fmt::format_to(std::back_inserter(textures), "{}:{:08x}:{}x{}:f{:04x}:m{}+{}:s{}+{}:w4{:08x}:sw{:08x}/{:08x}:l{};", unit,
					texture->physAddress, texture->width, texture->height, (uint32)texture->format,
					view->firstMip, view->numMip, view->firstSlice, view->numSlice, word4,
					samplerWord0, samplerWord1, (uint32)layout);
			}
		}

		LatteTextureView* colorView = LatteMRT::GetColorAttachment(0);
		LatteTexture* colorTexture = colorView ? colorView->baseTexture : nullptr;
		const auto& registers = LatteGPUState.contextNew;
		cemuLog_log(LogType::Force,
			"[COMPAT:DRAW] backend=Vulkan sample_ms={} frame={} dc={} vs={:016x} ps={:016x} mode={} count={} base={} inst={} idx={:08x} "
			"rt={:08x}:{}x{}:f{:04x} alpha={:08x}:{:.5f} color={:08x}:{:08x} depth={:08x} raster={:08x} "
			"scissor={},{},{},{} viewport={:.2f},{:.2f},{:.2f},{:.2f} descriptor={:016x} tex={}",
			*traceSample, LatteGPUState.frameCounter, LatteGPUState.drawCallCounter,
			vertexShader ? vertexShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
			(uint32)primitiveMode, count, baseVertex, instanceCount, indexDataMPTR,
			colorTexture ? colorTexture->physAddress : 0, colorTexture ? colorTexture->width : 0,
			colorTexture ? colorTexture->height : 0, colorTexture ? (uint32)colorTexture->format : 0,
			registers.SX_ALPHA_TEST_CONTROL.getRawValue(), registers.SX_ALPHA_REF.get_ALPHA_TEST_REF(),
			registers.CB_COLOR_CONTROL.getRawValue(), registers.CB_TARGET_MASK.getRawValue(),
			registers.DB_DEPTH_CONTROL.getRawValue(), registers.PA_SU_SC_MODE_CNTL.getRawValue(),
			registers.PA_SC_GENERIC_SCISSOR_TL.get_TL_X(), registers.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y(),
			registers.PA_SC_GENERIC_SCISSOR_BR.get_BR_X(), registers.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y(),
			registers.PA_CL_VPORT_XOFFSET.get_OFFSET(), registers.PA_CL_VPORT_YOFFSET.get_OFFSET(),
			registers.PA_CL_VPORT_XSCALE.get_SCALE(), registers.PA_CL_VPORT_YSCALE.get_SCALE(),
			pixelDS ? pixelDS->stateHash : 0, textures);
	}
	const bool emitRenderDocLabel = IsMN9TraceTarget() && IsDebugMarkersEnabled() &&
		vkCmdBeginDebugUtilsLabelEXT && vkCmdEndDebugUtilsLabelEXT;
	std::string renderDocLabel;
	if (emitRenderDocLabel)
	{
		renderDocLabel = fmt::format("MN9 frame={} draw={} vs={:016x} ps={:016x} mode={} count={} instances={}",
			LatteGPUState.frameCounter, LatteGPUState.drawCallCounter,
			vertexShader ? vertexShader->baseHash : 0, pixelShader ? pixelShader->baseHash : 0,
			(uint32)primitiveMode, count, instanceCount);
		VkDebugUtilsLabelEXT label{};
		label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
		label.pLabelName = renderDocLabel.c_str();
		label.color[0] = 0.15f;
		label.color[1] = 0.65f;
		label.color[2] = 0.95f;
		label.color[3] = 1.0f;
		vkCmdBeginDebugUtilsLabelEXT(m_state.currentCommandBuffer, &label);
	}
	if (hostIndexType != INDEX_TYPE::NONE)
		vkCmdDrawIndexed(m_state.currentCommandBuffer, hostIndexCount, instanceCount, 0, baseVertex, baseInstance);
	else
		vkCmdDraw(m_state.currentCommandBuffer, count, instanceCount, baseVertex, baseInstance);
	if (emitRenderDocLabel)
		vkCmdEndDebugUtilsLabelEXT(m_state.currentCommandBuffer);


	LatteStreamout_FinishDrawcall(m_useHostMemoryForCache);

	LatteGPUState.drawCallCounter++;
	if (traceRE) ++RevelationsVulkanTiming::frame.emitted;
}

// used in place of vertex/uniform caching when direct memory access is possible
void VulkanRenderer::draw_updateVertexBuffersDirectAccess()
{
	LatteFetchShader* parsedFetchShader = LatteSHRC_GetActiveFetchShader();
	if (!parsedFetchShader)
		return;
	for (auto& bufferGroup : parsedFetchShader->bufferGroups)
	{
		uint32 bufferIndex = bufferGroup.attributeBufferIndex;
		uint32 bufferBaseRegisterIndex = mmSQ_VTX_ATTRIBUTE_BLOCK_START + bufferIndex * 7;
		MPTR bufferAddress = LatteGPUState.contextRegister[bufferBaseRegisterIndex + 0];
		uint32 bufferSize = LatteGPUState.contextRegister[bufferBaseRegisterIndex + 1] + 1;
		uint32 bufferStride = (LatteGPUState.contextRegister[bufferBaseRegisterIndex + 2] >> 11) & 0xFFFF;

		if (bufferAddress == MPTR_NULL) [[unlikely]]
		{
			bufferAddress = 0x10000000;
		}
		if (m_state.currentVertexBinding[bufferIndex].offset == bufferAddress)
			continue;
		cemu_assert_debug(bufferAddress < 0x50000000);
		VkBuffer attrBuffer = m_importedMem;
		VkDeviceSize attrOffset = bufferAddress - m_importedMemBaseAddress;
		vkCmdBindVertexBuffers(m_state.currentCommandBuffer, bufferIndex, 1, &attrBuffer, &attrOffset);
	}
}

void VulkanRenderer::draw_updateUniformBuffersDirectAccess(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset, LatteConst::ShaderType shaderType)
{
	if (shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
	{
		for(const auto& buf : shader->list_quickBufferList)
		{
			sint32 i = buf.index;
			MPTR physicalAddr = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 0];
			uint32 uniformSize = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 1] + 1;

			if (physicalAddr == MPTR_NULL) [[unlikely]]
			{
				cemu_assert_unimplemented();
				continue;
			}
			uniformSize = std::min<uint32>(uniformSize, buf.size);

			cemu_assert_debug(physicalAddr < 0x50000000);

			uint32 bufferIndex = i;
			cemu_assert_debug(bufferIndex < 16);

			switch (shaderType)
			{
			case LatteConst::ShaderType::Vertex:
				dynamicOffsetInfo.shaderUB[VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX].uniformBufferOffset[bufferIndex] = physicalAddr - m_importedMemBaseAddress;
				break;
			case LatteConst::ShaderType::Geometry:
				dynamicOffsetInfo.shaderUB[VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY].uniformBufferOffset[bufferIndex] = physicalAddr - m_importedMemBaseAddress;
				break;
			case LatteConst::ShaderType::Pixel:
				dynamicOffsetInfo.shaderUB[VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT].uniformBufferOffset[bufferIndex] = physicalAddr - m_importedMemBaseAddress;
				break;
			default:
				UNREACHABLE;
			}
		}
	}
}

void VulkanRenderer::draw_endSequence()
{
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	// post-drawcall logic
	if (pixelShader)
		LatteRenderTarget_trackUpdates();
	bool hasReadback = LatteTextureReadback_Update();
	m_recordedDrawcalls++;
	if (m_recordedDrawcalls >= m_submitThreshold || hasReadback)
	{
		SubmitCommandBuffer();
	}
}

void VulkanRenderer::debug_genericBarrier()
{
	draw_endRenderPass();

	VkMemoryBarrier memoryBarrier{};
	memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memoryBarrier.srcAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
		VK_ACCESS_INDEX_READ_BIT |
		VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
		VK_ACCESS_UNIFORM_READ_BIT |
		VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
		VK_ACCESS_SHADER_READ_BIT |
		VK_ACCESS_SHADER_WRITE_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT |
		VK_ACCESS_HOST_READ_BIT |
		VK_ACCESS_HOST_WRITE_BIT |
		VK_ACCESS_MEMORY_READ_BIT |
		VK_ACCESS_MEMORY_WRITE_BIT;

	memoryBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
		VK_ACCESS_INDEX_READ_BIT |
		VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
		VK_ACCESS_UNIFORM_READ_BIT |
		VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
		VK_ACCESS_SHADER_READ_BIT |
		VK_ACCESS_SHADER_WRITE_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT |
		VK_ACCESS_HOST_READ_BIT |
		VK_ACCESS_HOST_WRITE_BIT |
		VK_ACCESS_MEMORY_READ_BIT |
		VK_ACCESS_MEMORY_WRITE_BIT;

	vkCmdPipelineBarrier(m_state.currentCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
}
