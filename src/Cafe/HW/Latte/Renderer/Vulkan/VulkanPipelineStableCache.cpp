#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineStableCache.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "config/ActiveSettings.h"
#include "util/helpers/Serializer.h"
#include "Cafe/HW/Latte/Common/RegisterSerializer.h"
#include "Cemu/FileCache/FileCache.h"
#include "Cafe/HW/Latte/Core/LatteShaderCache.h"
#include "util/helpers/helpers.h"
#include <openssl/sha.h>
#include "Cafe/CafeSystem.h"
#include "RevelationsPipelineReuse.h"
#include "RevelationsPipelineStateAudit.h"

namespace
{
RevelationsPipelineReuse::Claims s_revelationsClaims;
bool s_revelationsAudit{}, s_revelationsRetain{};
std::atomic_size_t s_revelationsBuilt{}, s_revelationsRetained{}, s_revelationsFailed{};
}

struct
{
	uint32 pipelineLoadIndex;
	uint32 pipelineMaxFileIndex;
	
	std::atomic_uint32_t pipelinesQueued;
	std::atomic_uint32_t pipelinesLoaded;
}g_vkCacheState;

VulkanPipelineStableCache g_vkPipelineStableCacheInstance;

VulkanPipelineStableCache& VulkanPipelineStableCache::GetInstance()
{
	return g_vkPipelineStableCacheInstance;
}

uint32 VulkanPipelineStableCache::BeginLoading(uint64 cacheTitleId)
{
	s_revelationsRetain = RevelationsPipelineReuse::Matches(true,
		CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
	s_revelationsAudit = RevelationsPipelineReuse::Matches(std::getenv("CEMU_RE_PIPELINE_REUSE_AUDIT") != nullptr,
		CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
	if (s_revelationsRetain || s_revelationsAudit)
	{
		s_revelationsClaims.Reset();
		s_revelationsBuilt = 0;
		s_revelationsRetained = 0;
		s_revelationsFailed = 0;
	}
	std::error_code ec;
	fs::create_directories(ActiveSettings::GetCachePath("shaderCache/transferable"), ec);
	const auto pathCacheFile = ActiveSettings::GetCachePath("shaderCache/transferable/{:016x}_vkpipeline.bin", cacheTitleId);
	
	// init cache loader state
	g_vkCacheState.pipelineLoadIndex = 0;
	g_vkCacheState.pipelineMaxFileIndex = 0;
	g_vkCacheState.pipelinesLoaded = 0;
	g_vkCacheState.pipelinesQueued = 0;
	
	// start async compilation threads
	m_compilationCount.store(0);	
	m_compilationQueue.clear();

	// get core count
	uint32 cpuCoreCount = GetPhysicalCoreCount();
	m_numCompilationThreads = std::clamp(cpuCoreCount, 1u, 8u);
	if (VulkanRenderer::GetInstance()->GetDisableMultithreadedCompilation())
		m_numCompilationThreads = 1;

	for (uint32 i = 0; i < m_numCompilationThreads; i++)
	{
		std::thread compileThread(&VulkanPipelineStableCache::CompilerThread, this);
		compileThread.detach();
	}

	// open cache file or create it
	cemu_assert_debug(s_cache == nullptr);
	s_cache = FileCache::Open(pathCacheFile, true, LatteShaderCache_getPipelineCacheExtraVersion(cacheTitleId));
	if (!s_cache)
	{
		cemuLog_log(LogType::Force, "Failed to open or create Vulkan pipeline cache file: {}", _pathToUtf8(pathCacheFile));
		return 0;
	}
	else
	{
		s_cache->UseCompression(false);
		g_vkCacheState.pipelineMaxFileIndex = s_cache->GetMaximumFileIndex();
	}
	return s_cache->GetFileCount();
}

bool VulkanPipelineStableCache::UpdateLoading(uint32& pipelinesLoadedTotal, uint32& pipelinesMissingShaders)
{
	pipelinesLoadedTotal = g_vkCacheState.pipelinesLoaded;
	pipelinesMissingShaders = 0;
	while (g_vkCacheState.pipelineLoadIndex <= g_vkCacheState.pipelineMaxFileIndex)
	{
		if (m_compilationQueue.size() >= 50)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			return true; // queue up to 50 entries at a time
		}

		uint64 fileNameA, fileNameB;
		std::vector<uint8> fileData;
		if (s_cache->GetFileByIndex(g_vkCacheState.pipelineLoadIndex, &fileNameA, &fileNameB, fileData))
		{
			// queue for async compilation
			g_vkCacheState.pipelinesQueued++;
			m_compilationQueue.push(std::move(fileData));
			g_vkCacheState.pipelineLoadIndex++;
			return true;
		}
		g_vkCacheState.pipelineLoadIndex++;
	}
	if (g_vkCacheState.pipelinesLoaded != g_vkCacheState.pipelinesQueued)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return true; // pipelines still compiling
	}
	return false; // done
}

void VulkanPipelineStableCache::EndLoading()
{
	if (s_revelationsRetain || s_revelationsAudit)
	{
		const auto stats = s_revelationsClaims.Snapshot();
		cemuLog_log(LogType::Force, "[RE:PIPELINE] event=summary retain={} records={} unique_runtime_keys={} duplicate_records={} built={} retained={} failed={}",
			s_revelationsRetain, stats.records, stats.unique, stats.duplicates,
			s_revelationsBuilt.load(), s_revelationsRetained.load(), s_revelationsFailed.load());
	}
	// shut down compilation threads
	uint32 threadCount = m_numCompilationThreads;
	m_numCompilationThreads = 0; // signal thread shutdown
	for (uint32 i = 0; i < threadCount; i++)
	{
		m_compilationQueue.push({}); // push empty workload for every thread. Threads then will shutdown after checking for m_numCompilationThreads == 0
	}
	// keep cache file open for writing of new pipelines
}

void VulkanPipelineStableCache::Close()
{
    if(s_cache)
    {
        delete s_cache;
        s_cache = nullptr;
    }
}

struct CachedPipeline
{
	struct ShaderHash
	{
		uint64 baseHash;
		uint64 auxHash;
		bool isPresent{};

		void set(uint64 baseHash, uint64 auxHash)
		{
			this->baseHash = baseHash;
			this->auxHash = auxHash;
			this->isPresent = true;
		}
	};

	ShaderHash vsHash; // includes fetch shader
	ShaderHash gsHash;
	ShaderHash psHash;

	Latte::GPUCompactedRegisterState gpuState;
};

VkFormat __getColorBufferVkFormat(const uint32 index, const LatteContextRegister& lcr)
{
	Latte::E_GX2SURFFMT colorBufferFormat = LatteMRT::GetColorBufferFormat(index, lcr);
	VulkanRenderer::FormatInfoVK texFormatInfo;
	VulkanRenderer::GetInstance()->GetTextureFormatInfoVK(colorBufferFormat, false, Latte::E_DIM::DIM_2D, 1280, 720, &texFormatInfo);
	return texFormatInfo.vkImageFormat;
}

void __getDepthBufferVkFormat(const LatteContextRegister& lcr, VkFormat& dbFormat, bool& hasStencil)
{
	Latte::E_GX2SURFFMT format = LatteMRT::GetDepthBufferFormat(lcr);
	VulkanRenderer::FormatInfoVK texFormatInfo;
	VulkanRenderer::GetInstance()->GetTextureFormatInfoVK(format, true, Latte::E_DIM::DIM_2D, 1280, 720, &texFormatInfo);
	dbFormat = texFormatInfo.vkImageFormat;
	hasStencil = (texFormatInfo.vkImageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
}

// create placeholder renderpass for cached pipeline
VKRObjectRenderPass* __CreateTemporaryRenderPass(const LatteDecompilerShader* pixelShader, const LatteContextRegister& lcr)
{
	VKRObjectRenderPass::AttachmentInfo_t attachmentInfo;

	uint8 cbMask = LatteMRT::GetActiveColorBufferMask(pixelShader, lcr);
	bool dbMask = LatteMRT::GetActiveDepthBufferMask(lcr);

	for (int i = 0; i < 8; ++i)
	{
		if ((cbMask & (1 << i)) == 0)
		{
			attachmentInfo.colorAttachment[i].viewObj = nullptr;
			continue;
		}
		// setup color attachment
		attachmentInfo.colorAttachment[i].viewObj = nullptr;
		attachmentInfo.colorAttachment[i].isPresent = true;
		attachmentInfo.colorAttachment[i].format = __getColorBufferVkFormat(i, lcr);
	}

	// setup depth attachment
	if (dbMask)
	{
		attachmentInfo.depthAttachment.viewObj = nullptr;
		attachmentInfo.depthAttachment.isPresent = true;
		VkFormat dbFormat;
		bool hasStencil;
		__getDepthBufferVkFormat(lcr, dbFormat, hasStencil);
		attachmentInfo.depthAttachment.format = dbFormat;
		attachmentInfo.depthAttachment.hasStencil = hasStencil;
	}
	else
	{
		// no depth attachment
		attachmentInfo.depthAttachment.viewObj = nullptr;
		attachmentInfo.depthAttachment.isPresent = false;
	}

	return new VKRObjectRenderPass(attachmentInfo);
}

void VulkanPipelineStableCache::LoadPipelineFromCache(std::span<uint8> fileData)
{
	static FSpinlock s_spinlockSharedInternal;

	// deserialize file
	LatteContextRegister* lcr = new LatteContextRegister();
	s_spinlockSharedInternal.lock();
	CachedPipeline* cachedPipeline = new CachedPipeline();
	s_spinlockSharedInternal.unlock();

	MemStreamReader streamReader(fileData.data(), fileData.size());
	if (!DeserializePipeline(streamReader, *cachedPipeline))
	{
		// failed to deserialize
		s_spinlockSharedInternal.lock();
		delete lcr;
		delete cachedPipeline;
		s_spinlockSharedInternal.unlock();
		return;
	}
	// restored register view from compacted state
	Latte::LoadGPURegisterState(*lcr, cachedPipeline->gpuState);

	LatteDecompilerShader* vertexShader = nullptr;
	LatteDecompilerShader* geometryShader = nullptr;
	LatteDecompilerShader* pixelShader = nullptr;
	// find vertex shader
	if (cachedPipeline->vsHash.isPresent)
	{
		vertexShader = LatteSHRC_FindVertexShader(cachedPipeline->vsHash.baseHash, cachedPipeline->vsHash.auxHash);
		if (!vertexShader)
		{
			cemuLog_logDebug(LogType::Force, "Vertex shader not found in cache");
			return;
		}
	}
	// find geometry shader
	if (cachedPipeline->gsHash.isPresent)
	{
		geometryShader = LatteSHRC_FindGeometryShader(cachedPipeline->gsHash.baseHash, cachedPipeline->gsHash.auxHash);
		if (!geometryShader)
		{
			cemuLog_logDebug(LogType::Force, "Geometry shader not found in cache");
			return;
		}
	}
	// find pixel shader
	if (cachedPipeline->psHash.isPresent)
	{
		pixelShader = LatteSHRC_FindPixelShader(cachedPipeline->psHash.baseHash, cachedPipeline->psHash.auxHash);
		if (!pixelShader)
		{
			cemuLog_logDebug(LogType::Force, "Pixel shader not found in cache");
			return;
		}
	}
	// create temporary renderpass
	if (!pixelShader)
	{
		cemu_assert_debug(false);
		return;
	}
	auto renderPass = __CreateTemporaryRenderPass(pixelShader, *lcr);
	const uint64 pipelineBaseHash = vertexShader->baseHash;
	const uint64 pipelineStateHash = VulkanRenderer::draw_calculateGraphicsPipelineHash(vertexShader->compatibleFetchShader, vertexShader, geometryShader, pixelShader, renderPass, *lcr);
	const RevelationsPipelineReuse::Key reuseKey{pipelineBaseHash, pipelineStateHash};
	bool ownsPreparation = true;
	if (s_revelationsRetain || s_revelationsAudit)
		ownsPreparation = s_revelationsClaims.Observe(reuseKey);
	if (s_revelationsAudit)
	{
		uint8 digest[SHA256_DIGEST_LENGTH];
		SHA256(fileData.data(), fileData.size(), digest);
		uint64 recordA = 0, recordB = 0;
		for (unsigned i = 0; i < 8; ++i) { recordA = (recordA << 8) | digest[i]; recordB = (recordB << 8) | digest[i + 8]; }
		cemuLog_log(LogType::Force, "[RE:PIPELINE] event=record record={:016x}{:016x} base={:016x} state={:016x} duplicate={}",
			recordA, recordB, pipelineBaseHash, pipelineStateHash, !ownsPreparation);
	}
	if (s_revelationsRetain && !ownsPreparation)
	{
		// The owner is already preparing this key. Startup waits for every owner;
		// failed owners leave a normal runtime cache miss rather than a bad pipeline.
		s_spinlockSharedInternal.lock();
		delete lcr;
		delete cachedPipeline;
		VulkanRenderer::GetInstance()->ReleaseDestructibleObject(renderPass);
		s_spinlockSharedInternal.unlock();
		return;
	}
	// create pipeline info
	if (s_revelationsRetain || s_revelationsAudit) s_spinlockSharedInternal.lock(); else m_pipelineIsCachedLock.lock();
	const uint64 minimalHash = s_revelationsRetain ? VulkanRenderer::draw_calculateMinimalGraphicsPipelineHash(vertexShader->compatibleFetchShader, *lcr) : 0;
	PipelineInfo* pipelineInfo = new PipelineInfo(minimalHash, s_revelationsRetain ? pipelineStateHash : 0, vertexShader->compatibleFetchShader, vertexShader, pixelShader, geometryShader);
	if (s_revelationsRetain || s_revelationsAudit) s_spinlockSharedInternal.unlock(); else m_pipelineIsCachedLock.unlock();
	// compile
	{
		PipelineCompiler pipelineCompiler;
		bool requiresRobustBufferAccess = PipelineCompiler::CalcRobustBufferAccessRequirement(vertexShader, pixelShader, geometryShader);
		if (!pipelineCompiler.InitFromCurrentGPUState(pipelineInfo, *lcr, renderPass, requiresRobustBufferAccess))
		{
			if (s_revelationsRetain || s_revelationsAudit) { s_revelationsClaims.Failed(reuseKey); ++s_revelationsFailed; }
			s_spinlockSharedInternal.lock();
			delete lcr;
			delete cachedPipeline;
			s_spinlockSharedInternal.unlock();
			return;
		}
		pipelineCompiler.Compile(true, true, false);
		if (s_revelationsAudit)
		{
			const auto state = RevelationsPipelineReuse::CreationState(pipelineCompiler, *pipelineInfo, renderPass->m_hashForPipeline);
			uint8 digest[SHA256_DIGEST_LENGTH];
			SHA256(reinterpret_cast<const uint8*>(state.data()), state.size(), digest);
			std::string fingerprint;
			for (const auto byte : digest) fingerprint += fmt::format("{:02x}", byte);
			cemuLog_log(LogType::Force, "[RE:PIPELINE] event=creation base={:016x} state={:016x} fingerprint={}",
				pipelineBaseHash, pipelineStateHash, fingerprint);
		}
	}
	const bool built = pipelineInfo->m_vkrObjPipeline->GetPipeline() != VK_NULL_HANDLE;
	if (s_revelationsRetain || s_revelationsAudit)
	{
		if (built) ++s_revelationsBuilt;
		else { s_revelationsClaims.Failed(reuseKey); ++s_revelationsFailed; }
	}
	// on success, calculate pipeline hash and flag as present in cache
	m_pipelineIsCachedLock.lock();
	m_pipelineIsCached.emplace(pipelineBaseHash, pipelineStateHash);
	m_pipelineIsCachedLock.unlock();
	// clean up
	s_spinlockSharedInternal.lock();
	bool retained = false;
	if (s_revelationsRetain && built)
	{
		auto& map = VulkanRenderer::GetInstance()->m_pipeline_info_cache[pipelineBaseHash];
		retained = map.emplace(pipelineStateHash, pipelineInfo).second;
		if (retained) ++s_revelationsRetained;
	}
	if (!retained) delete pipelineInfo;
	delete lcr;
	delete cachedPipeline;
	VulkanRenderer::GetInstance()->ReleaseDestructibleObject(renderPass);
	s_spinlockSharedInternal.unlock();
}

bool VulkanPipelineStableCache::HasPipelineCached(uint64 baseHash, uint64 pipelineStateHash)
{
	PipelineHash ph(baseHash, pipelineStateHash);
	return m_pipelineIsCached.find(ph) != m_pipelineIsCached.end();
}

ConcurrentQueue<CachedPipeline*> g_pipelineCachingQueue;

void VulkanPipelineStableCache::AddCurrentStateToCache(uint64 baseHash, uint64 pipelineStateHash)
{
	m_pipelineIsCached.emplace(baseHash, pipelineStateHash);
	if (!m_pipelineCacheStoreThread)
	{
		m_pipelineCacheStoreThread = new std::thread(&VulkanPipelineStableCache::WorkerThread, this);
		m_pipelineCacheStoreThread->detach();
	}
	// fill job structure with cached GPU state
	// for each cached pipeline we store:
	// - Active shaders (referenced by hash)
	// - An almost-complete register state of the GPU (minus some ALU uniform constants which aren't relevant)
	CachedPipeline* job = new CachedPipeline();
	auto vs = LatteSHRC_GetActiveVertexShader();
	auto gs = LatteSHRC_GetActiveGeometryShader();
	auto ps = LatteSHRC_GetActivePixelShader();
	if (vs)
		job->vsHash.set(vs->baseHash, vs->auxHash);
	if (gs)
		job->gsHash.set(gs->baseHash, gs->auxHash);
	if (ps)
		job->psHash.set(ps->baseHash, ps->auxHash);
	Latte::StoreGPURegisterState(LatteGPUState.contextNew, job->gpuState);
	// queue job
	g_pipelineCachingQueue.push(job);
}

bool VulkanPipelineStableCache::SerializePipeline(MemStreamWriter& memWriter, CachedPipeline& cachedPipeline)
{
	memWriter.writeBE<uint8>(0x01); // version
	uint8 presentMask = 0;
	if (cachedPipeline.vsHash.isPresent)
		presentMask |= 1;
	if (cachedPipeline.gsHash.isPresent)
		presentMask |= 2;
	if (cachedPipeline.psHash.isPresent)
		presentMask |= 4;
	memWriter.writeBE<uint8>(presentMask);
	if (cachedPipeline.vsHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.vsHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.vsHash.auxHash);
	}
	if (cachedPipeline.gsHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.gsHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.gsHash.auxHash);
	}
	if (cachedPipeline.psHash.isPresent)
	{
		memWriter.writeBE<uint64>(cachedPipeline.psHash.baseHash);
		memWriter.writeBE<uint64>(cachedPipeline.psHash.auxHash);
	}
	Latte::SerializeRegisterState(cachedPipeline.gpuState, memWriter);
	return true;
}

bool VulkanPipelineStableCache::DeserializePipeline(MemStreamReader& memReader, CachedPipeline& cachedPipeline)
{
	// version
	if (memReader.readBE<uint8>() != 1)
	{
		cemuLog_log(LogType::Force, "Cached Vulkan pipeline corrupted or has unknown version");
		return false;
	}
	// shader hashes
	uint8 presentMask = memReader.readBE<uint8>();
	if (presentMask & 1)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.vsHash.set(baseHash, auxHash);
	}
	if (presentMask & 2)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.gsHash.set(baseHash, auxHash);
	}
	if (presentMask & 4)
	{
		uint64 baseHash = memReader.readBE<uint64>();
		uint64 auxHash = memReader.readBE<uint64>();
		cachedPipeline.psHash.set(baseHash, auxHash);
	}
	// deserialize GPU state
	if (!Latte::DeserializeRegisterState(cachedPipeline.gpuState, memReader))
	{
		return false;
	}
	cemu_assert_debug(!memReader.hasError());
	return true;
}

int VulkanPipelineStableCache::CompilerThread()
{
	SetThreadName("plCacheCompiler");
	while (m_numCompilationThreads != 0)
	{
		std::vector<uint8> pipelineData = m_compilationQueue.pop();
		if(pipelineData.empty())
			continue;
		LoadPipelineFromCache(pipelineData);
		++g_vkCacheState.pipelinesLoaded;
	}
	return 0;
}

void VulkanPipelineStableCache::WorkerThread()
{
	SetThreadName("plCacheWriter");
	while (true)
	{
		CachedPipeline* job;
		g_pipelineCachingQueue.pop(job);
		if (!s_cache)
		{
			delete job;
			continue;
		}
		// serialize
		MemStreamWriter memWriter(1024 * 4);
		SerializePipeline(memWriter, *job);
		auto blob = memWriter.getResult();
		// file name is derived from data hash
		uint8 hash[SHA256_DIGEST_LENGTH];
		SHA256(blob.data(), blob.size(), hash);
		uint64 nameA = *(uint64be*)(hash + 0);
		uint64 nameB = *(uint64be*)(hash + 8);
		s_cache->AddFileAsync({ nameA, nameB }, blob.data(), blob.size());
		delete job;
	}
}
