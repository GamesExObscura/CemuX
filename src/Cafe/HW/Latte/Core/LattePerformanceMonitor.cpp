#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "WindowSystem.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/RevelationsShaderScheduling.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/RevelationsVulkanTiming.h"

performanceMonitor_t performanceMonitor{};

void LattePerformanceMonitor_frameEnd()
{
	const auto vkTiming = RevelationsVulkanTiming::TakeFrame();
	// per-frame stats
	performanceMonitor.gpuTime_shaderCreate.frameFinished();
	performanceMonitor.gpuTime_frameTime.frameFinished();
	performanceMonitor.gpuTime_idleTime.frameFinished();
	performanceMonitor.gpuTime_fenceTime.frameFinished();

	performanceMonitor.gpuTime_dcStageTextures.frameFinished();
	performanceMonitor.gpuTime_dcStageVertexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageShaderAndUniformMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageIndexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageMRT.frameFinished();
	performanceMonitor.gpuTime_dcStageDrawcallAPI.frameFinished();
	performanceMonitor.gpuTime_waitForAsync.frameFinished();
	static const bool traceRequested = std::getenv("CEMU_RE_FRAME_TIMING") != nullptr;
	static uint64 traceStart{}, previousTick{}, traceFrame{};
	if (traceRequested && RevelationsShaderScheduling::Matches(CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()))
	{
		const uint64 tick = PPCTimer_getRawTsc();
		if (!previousTick) traceStart = tick;
		const uint64 gapUs = previousTick ? PPCTimer_tscToMicroseconds(tick - previousTick) : 0;
		++traceFrame;
		if (traceFrame == 480 && std::getenv("CEMU_RE_VK_CREATE_AUDIT"))
		{
			const auto& total = RevelationsVulkanTiming::cumulative;
			cemuLog_log(LogType::Force, "[RE:VKTOTAL] frame=480 shader_setup_ms={:.3f} pipeline_ms={:.3f} emitted={} null_pipelines={} skipped_sequences={}",
				total.nanoseconds[static_cast<unsigned>(RevelationsVulkanTiming::Stage::Shader)] / 1000000.0,
				total.nanoseconds[static_cast<unsigned>(RevelationsVulkanTiming::Stage::Pipeline)] / 1000000.0,
				total.emitted, total.nullPipelines, total.skippedSequences);
		}
		if (previousTick && (gapUs >= 45000 || traceFrame % 120 == 0 || vkTiming.nullPipelines || vkTiming.skippedSequences))
		{
			const auto ms = [](LattePerfStatTimer& timer) { return PPCTimer_tscToMicroseconds(timer.getPreviousFrameValue()) / 1000.0; };
			cemuLog_log(LogType::Force, "[RE:FRAME] frame={} elapsed_ms={:.3f} gap_ms={:.3f} native_decompile_ms={:.3f} shader_manager_ms={:.3f} textures_ms={:.3f} api_ms={:.3f} gpu_fence_ms={:.3f} async_wait_ms={:.3f} guest_idle_ms={:.3f}",
				traceFrame, PPCTimer_tscToMicroseconds(tick - traceStart) / 1000.0, gapUs / 1000.0,
				ms(performanceMonitor.gpuTime_shaderCreate), ms(performanceMonitor.gpuTime_dcStageShaderAndUniformMgr),
				ms(performanceMonitor.gpuTime_dcStageTextures), ms(performanceMonitor.gpuTime_dcStageDrawcallAPI),
				ms(performanceMonitor.gpuTime_fenceTime), ms(performanceMonitor.gpuTime_waitForAsync), ms(performanceMonitor.gpuTime_idleTime));
			if (std::getenv("CEMU_RE_VK_TIMING"))
			{
				const auto vkMs = [&](RevelationsVulkanTiming::Stage stage) {
					return vkTiming.nanoseconds[static_cast<unsigned>(stage)] / 1000000.0;
				};
				cemuLog_log(LogType::Force, "[RE:VKFRAME] frame={} shader_setup_ms={:.3f} texture_mrt_ms={:.3f} buffers_ms={:.3f} pipeline_ms={:.3f} descriptors_ms={:.3f} submit_ms={:.3f} emitted={} null_pipelines={} skipped_sequences={}",
					traceFrame, vkMs(RevelationsVulkanTiming::Stage::Shader), vkMs(RevelationsVulkanTiming::Stage::Textures),
					vkMs(RevelationsVulkanTiming::Stage::Buffers), vkMs(RevelationsVulkanTiming::Stage::Pipeline),
					vkMs(RevelationsVulkanTiming::Stage::Descriptors), vkMs(RevelationsVulkanTiming::Stage::Submit),
					vkTiming.emitted, vkTiming.nullPipelines, vkTiming.skippedSequences);
			}
		}
		previousTick = tick;
	}
	else
	{
		traceStart = previousTick = traceFrame = 0;
	}

	uint32 elapsedTime = GetTickCount() - performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate;
	if (elapsedTime >= 1000)
	{
		bool isFirstUpdate = performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate == 0;
		// sum up raw stats
		uint32 totalElapsedTime = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 totalElapsedTimeFPS = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 elapsedFrames = 0;
		uint32 elapsedFrames2S = 0; // elapsed frames for last two entries (seconds)
		uint64 skippedCycles = 0;
		uint64 vertexDataUploaded = 0;
		uint64 vertexDataCached = 0;
		uint64 uniformBankUploadedData = 0;
		uint64 uniformBankUploadedCount = 0;
		uint64 indexDataUploaded = 0;
		uint64 indexDataCached = 0;
		uint32 frameCounter = 0;
		uint32 drawCallCounter = 0;
		uint32 fastDrawCallCounter = 0;
		uint32 shaderBindCounter = 0;
		uint32 recompilerLeaveCount = 0;
		uint32 threadLeaveCount = 0;
		for (sint32 i = 0; i < PERFORMANCE_MONITOR_TRACK_CYCLES; i++)
		{
			elapsedFrames += performanceMonitor.cycle[i].frameCounter;
			skippedCycles += performanceMonitor.cycle[i].skippedCycles;
			vertexDataUploaded += performanceMonitor.cycle[i].vertexDataUploaded;
			vertexDataCached += performanceMonitor.cycle[i].vertexDataCached;
			uniformBankUploadedData += performanceMonitor.cycle[i].uniformBankUploadedData;
			uniformBankUploadedCount += performanceMonitor.cycle[i].uniformBankUploadedCount;
			indexDataUploaded += performanceMonitor.cycle[i].indexDataUploaded;
			indexDataCached += performanceMonitor.cycle[i].indexDataCached;
			frameCounter += performanceMonitor.cycle[i].frameCounter;
			drawCallCounter += performanceMonitor.cycle[i].drawCallCounter;
			fastDrawCallCounter += performanceMonitor.cycle[i].fastDrawCallCounter;
			shaderBindCounter += performanceMonitor.cycle[i].shaderBindCount;
			recompilerLeaveCount += performanceMonitor.cycle[i].recompilerLeaveCount;
			threadLeaveCount += performanceMonitor.cycle[i].threadLeaveCount;
		}
		elapsedFrames = std::max<uint32>(elapsedFrames, 1);
		elapsedFrames2S = performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 0) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S += performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S = std::max<uint32>(elapsedFrames2S, 1);
		// calculate stats
		uint64 passedCycles = PPCInterpreter_getMainCoreCycleCounter() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastCycleCount;
		passedCycles -= skippedCycles;
		uint64 vertexDataUploadPerFrame = (vertexDataUploaded / (uint64)elapsedFrames);
		vertexDataUploadPerFrame /= 1024ULL;
		uint64 vertexDataCachedPerFrame = (vertexDataCached / (uint64)elapsedFrames);
		vertexDataCachedPerFrame /= 1024ULL;
		uint64 uniformBankDataUploadedPerFrame = (uniformBankUploadedData / (uint64)elapsedFrames);
		uniformBankDataUploadedPerFrame /= 1024ULL;
		uint32 uniformBankCountUploadedPerFrame = (uint32)(uniformBankUploadedCount / (uint64)elapsedFrames);
		uint64 indexDataUploadPerFrame = (indexDataUploaded / (uint64)elapsedFrames);

		double fps = (double)elapsedFrames2S * 1000.0 / (double)totalElapsedTimeFPS;
		uint32 shaderBindsPerFrame = shaderBindCounter / elapsedFrames;
		passedCycles = passedCycles * 1000ULL / totalElapsedTime;
		uint32 rlps = (uint32)((uint64)recompilerLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		uint32 tlps = (uint32)((uint64)threadLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		// set stats
		performanceMonitor.stats.indexDataUploadPerFrame = indexDataUploadPerFrame;
		// next counter cycle
		sint32 nextCycleIndex = (performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES;
		performanceMonitor.cycle[nextCycleIndex].drawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].fastDrawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].frameCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].shaderBindCount = 0;
		performanceMonitor.cycle[nextCycleIndex].lastCycleCount = PPCInterpreter_getMainCoreCycleCounter();
		performanceMonitor.cycle[nextCycleIndex].skippedCycles = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedData = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedCount = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].recompilerLeaveCount = 0;
		performanceMonitor.cycle[nextCycleIndex].threadLeaveCount = 0;
		performanceMonitor.cycleIndex = nextCycleIndex;

		// next update in 1 second
		performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate = GetTickCount();

		if (isFirstUpdate)
		{
			LatteOverlay_updateStats(0.0, 0, 0);
			WindowSystem::UpdateWindowTitles(false, false, 0.0);
		}
		else
		{
			LatteOverlay_updateStats(fps, drawCallCounter / elapsedFrames, fastDrawCallCounter / elapsedFrames);
			WindowSystem::UpdateWindowTitles(false, false, fps);
		}
	}
}

void LattePerformanceMonitor_frameBegin()
{
	performanceMonitor.vk.numDrawBarriersPerFrame.reset();
	performanceMonitor.vk.numBeginRenderpassPerFrame.reset();
}
