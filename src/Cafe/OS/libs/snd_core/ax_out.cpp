#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/CafeSystem.h"
#include "audio/IAudioAPI.h"
//#include "ax.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "SpongeAudioTiming.h"
#include "DigAudioTiming.h"
#include "Cafe/TerrariaCompatibility.h"
#include "WheelAudioTiming.h"
#include "Cafe/OS/libs/coreinit/HomeSession.h"
#include "Cafe/NbaHomeAudio.h"

namespace snd_core
{
	namespace
	{
		bool IsDigAudioCatchUpTarget()
		{
			return DigAudioTiming::Enabled(CafeSystem::GetForegroundTitleId(),
				CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
		}

		bool IsTerrariaAudioCatchUpTarget()
		{
			return TerrariaCompatibility::Enabled(CafeSystem::GetForegroundTitleId(),
				CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
		}

		bool IsSpongeAudioCatchUpTarget()
		{
			return SpongeAudioTiming::Enabled(CafeSystem::GetForegroundTitleId(),
				CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
		}

		bool IsRaymanAudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x000500001010b100ull &&
				CafeSystem::GetForegroundTitleVersion() == 0 &&
				CafeSystem::GetRPXHashUpdated() == 0x1e9e626a;
		}

		bool IsRabbidsAudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x000500001010b000ull &&
				CafeSystem::GetForegroundTitleVersion() == 16 &&
				CafeSystem::GetRPXHashUpdated() == 0xb92e71ae;
		}

		bool IsNbaAudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x000500001010da00ull &&
				CafeSystem::GetForegroundTitleVersion() == 32 &&
				CafeSystem::GetRPXHashUpdated() == 0xef62d66e;
		}

		bool IsMN9AudioTraceTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x00050000101C9600ull &&
				CafeSystem::GetForegroundTitleVersion() == 16 &&
				CafeSystem::GetRPXHashUpdated() == 0x73E4FFC8;
		}

		bool IsNinjaAudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x0005000010110a00ULL &&
				CafeSystem::GetForegroundTitleVersion() == 80 &&
				CafeSystem::GetRPXHashUpdated() == 0x35c71348 &&
				ActiveSettings::GetGraphicsAPI() == GraphicAPI::kOpenGL;
		}

		bool IsJD2014AudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x0005000010139900ull &&
				CafeSystem::GetForegroundTitleVersion() == 16 &&
				CafeSystem::GetRPXHashUpdated() == 0x220D9596;
		}

		bool IsMarioParty10AudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x0005000010162d00ull &&
				CafeSystem::GetForegroundTitleVersion() == 16 &&
				CafeSystem::GetRPXHashUpdated() == 0xB9064400;
		}

		bool IsAvengersBattleForEarthAudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x000500001010af00ull &&
				CafeSystem::GetForegroundTitleVersion() == 16 &&
				CafeSystem::GetRPXHashUpdated() == 0xb57ad7ba;
		}

		bool IsJD2015AudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x0005000010179100ull &&
				CafeSystem::GetForegroundTitleVersion() == 16 &&
				CafeSystem::GetRPXHashUpdated() == 0x9592379F;
		}

		bool IsJD2016AudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x00050000101b9000ull &&
				CafeSystem::GetForegroundTitleVersion() == 32 &&
				CafeSystem::GetRPXHashUpdated() == 0x18c43164;
		}

		bool IsJD2017AudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x00050000101eb200ull &&
				CafeSystem::GetForegroundTitleVersion() == 16 &&
				CafeSystem::GetRPXHashUpdated() == 0x123f595d;
		}

		bool IsJD2018AudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x0005000010211300ull &&
				CafeSystem::GetForegroundTitleVersion() == 0 &&
				CafeSystem::GetRPXHashUpdated() == 0x1774ab26;
		}

		bool IsJD2019AudioCatchUpTarget()
		{
			return CafeSystem::GetForegroundTitleId() == 0x0005000010217300ull &&
				CafeSystem::GetForegroundTitleVersion() == 64 &&
				CafeSystem::GetRPXHashUpdated() == 0xd29b0f62;
		}

		std::atomic<uint64> s_mn9TVFeedAccepted{};
		std::atomic<uint64> s_mn9TVFeedRejected{};
		std::atomic<uint64> s_mn9DRCFeedAccepted{};
		std::atomic<uint64> s_mn9DRCFeedRejected{};
	}

	uint32 numProcessedFrames = 0;

	void resetNumProcessedFrames()
	{
		numProcessedFrames = 0;
	}

	uint32 getNumProcessedFrames()
	{
		return numProcessedFrames;
	}

	sint32 __AXMode[AX_DEV_COUNT]; // audio mode (AX_MODE_*) per device


	bool AVMGetTVAudioMode(uint32be* tvAudioMode)
	{
		// 0 -> mono
		// 1,2 -> stereo
		// 3 -> surround
		// 4 -> unknown mode
		switch (GetConfig().tv_channels)
		{
		case kMono:
			*tvAudioMode = 0;
			break;
		case kSurround:
			*tvAudioMode = 3;
			break;
		default:
			*tvAudioMode = 2;
			break;
		}

		return true;
	}

	bool AVMGetDRCSystemAudioMode(uint32be* drcAudioMode)
	{
		*drcAudioMode = 1; // apparently the default is Stereo(?), MH3U exits if AXGetDeviceMode doesn't return 0 (DRCSystemAudioMode must return 1 to set DRC mode to 0)
		return true;
	}

	sint32 __AXOutTVOutputChannelCount;
	sint32 __AXOutDRCOutputChannelCount;

	void __AXSetTVMode(sint32 mode)
	{
		cemu_assert(mode == AX_MODE_STEREO || mode == AX_MODE_6CH || mode == AX_MODE_MONO);
		__AXMode[AX_DEV_TV] = mode;
	}

	void __AXSetDeviceMode(sint32 device, sint32 mode)
	{
		if (device == AX_DEV_TV)
			__AXMode[AX_DEV_TV] = mode;
		else if (device == AX_DEV_DRC)
			__AXMode[AX_DEV_DRC] = mode;
		else if (device == AX_DEV_RMT)
			__AXMode[AX_DEV_RMT] = mode;
		else
		{
			cemu_assert_debug(false);
		}
	}

	sint32 AXGetDeviceMode(sint32 device)
	{
		if (device == AX_DEV_TV || device == AX_DEV_DRC || device == AX_DEV_RMT)
			return __AXMode[device];
		cemu_assert_debug(false);
		return 0;
	}

	void _AXOutInitDeviceModes()
	{
		// TV mode
		uint32be tvAudioMode;
		AVMGetTVAudioMode(&tvAudioMode);
		if (tvAudioMode == 0)
		{
			// mono
			__AXSetTVMode(AX_MODE_MONO);
			__AXOutTVOutputChannelCount = 1;
		}
		else if (tvAudioMode == 1 || tvAudioMode == 2)
		{
			// stereo
			__AXSetTVMode(AX_MODE_STEREO);
			__AXOutTVOutputChannelCount = 2;
		}
		else if (tvAudioMode == 3)
		{
			// surround (6ch)
			__AXSetTVMode(AX_MODE_6CH);
			__AXOutTVOutputChannelCount = 6;
		}
		else
		{
			assert_dbg();
		}
		// DRC mode
		uint32be drcAudioMode;
		AVMGetDRCSystemAudioMode(&drcAudioMode);
		if (drcAudioMode == 0)
		{
			// mono
			__AXSetDeviceMode(1, AX_MODE_MONO);
			__AXOutDRCOutputChannelCount = 1;
		}
		else if (drcAudioMode == 2)
		{
			// surround
			__AXSetDeviceMode(1, AX_MODE_SURROUND);
			__AXOutDRCOutputChannelCount = 2; // output channel count still 2 for DRC 'surround'
		}
		else if (drcAudioMode == 1)
		{
			// stereo
			__AXSetDeviceMode(1, AX_MODE_STEREO);
			__AXOutDRCOutputChannelCount = 2;
		}
		else
		{
			assert_dbg();
		}
	}

	void AXOut_Init()
	{
		_AXOutInitDeviceModes();
	}

	extern SysAllocator<sint32, AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT> __AXTVBuffer48;
	extern SysAllocator<sint32, AX_SAMPLES_MAX* AX_DRC_CHANNEL_COUNT * 2> __AXDRCBuffer48;

	sint16 __buf_AXTVDMABuffers_0[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16 __buf_AXTVDMABuffers_1[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16 __buf_AXTVDMABuffers_2[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16* __AXTVDMABuffers[3] = {__buf_AXTVDMABuffers_0, __buf_AXTVDMABuffers_1, __buf_AXTVDMABuffers_2};

	#define AX_FRAMES_PER_GROUP		(4)

	sint16 tempTVChannelData[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT * AX_FRAMES_PER_GROUP] = {};
	sint32 tempAudioBlockCounter = 0;


	sint16 __buf_AXDRCDMABuffers_0[AX_SAMPLES_MAX * 6];
	sint16 __buf_AXDRCDMABuffers_1[AX_SAMPLES_MAX * 6];
	sint16 __buf_AXDRCDMABuffers_2[AX_SAMPLES_MAX * 6];
	sint16* __AXDRCDMABuffers[3] = { __buf_AXDRCDMABuffers_0, __buf_AXDRCDMABuffers_1, __buf_AXDRCDMABuffers_2 };

	sint16 tempDRCChannelData[AX_SAMPLES_MAX * 6 * AX_FRAMES_PER_GROUP] = {};
	sint32 tempDRCAudioBlockCounter = 0;

	void AIInitDMA(sint16* sampleData, sint32 size)
	{
		sint32 sampleCount = size / sizeof(sint16); // sample count in total (summed up for all channels)

		if (sndGeneric.initParam.frameLength != 0)
		{
			cemu_assert(false);
		}

		std::shared_lock lock(g_audioMutex);

		const uint32 channels = g_tvAudio ? g_tvAudio->GetChannels() : AX_TV_CHANNEL_COUNT;
		sint16* outputChannel = tempTVChannelData + AX_SAMPLES_PER_3MS_48KHZ * tempAudioBlockCounter * channels;
		for (sint32 i = 0; i < sampleCount; ++i)
		{
			outputChannel[i] = _swapEndianS16(sampleData[i]);
		}

		tempAudioBlockCounter++;
		if (tempAudioBlockCounter == AX_FRAMES_PER_GROUP)
		{
			if(g_tvAudio)
			{
				const bool accepted = g_tvAudio->FeedBlock(tempTVChannelData);
				if (IsMN9AudioTraceTarget())
					(accepted ? s_mn9TVFeedAccepted : s_mn9TVFeedRejected).fetch_add(1, std::memory_order_relaxed);
			}

			tempAudioBlockCounter = 0;
		}
	}

	sint32 AIGetSamplesPerChannel(uint32 device)
	{
		// TV and DRC output the same number of samples
		return AX_SAMPLES_PER_3MS_48KHZ;
	}

	sint32 AIGetChannelCount(uint32 device)
	{
		if (__AXMode[device] == AX_MODE_6CH)
			return 6;
		if (__AXMode[device] == AX_MODE_STEREO)
			return 2;
		// default to mono
		return 1;
	}

	sint16* AIGetCurrentDMABuffer(uint32 device)
	{
		if (device == AX_DEV_TV)
			return __AXTVDMABuffers[0];
		else if (device == AX_DEV_DRC)
			return __AXDRCDMABuffers[0];
		cemu_assert_debug(false);
		return nullptr;
	}

	void AXOut_SubmitTVFrame(sint32 frameIndex)
	{
		sint32 numSamples = AIGetSamplesPerChannel(AX_DEV_TV);
		if (__AXMode[AX_DEV_TV] == AX_MODE_6CH)
		{
			sint32* inputChannel0 = __AXTVBuffer48.GetPtr() + numSamples * 0;
			sint32* inputChannel1 = __AXTVBuffer48.GetPtr() + numSamples * 1;
			sint32* inputChannel2 = __AXTVBuffer48.GetPtr() + numSamples * 2;
			sint32* inputChannel3 = __AXTVBuffer48.GetPtr() + numSamples * 3;
			sint32* inputChannel4 = __AXTVBuffer48.GetPtr() + numSamples * 4;
			sint32* inputChannel5 = __AXTVBuffer48.GetPtr() + numSamples * 5;
			sint16* dmaOutputBuffer = AIGetCurrentDMABuffer(AX_DEV_TV);
			for (sint32 i = 0; i < numSamples; i++)
			{
				/*
				* DirectSound surround order
				LEFT				0
				RIGHT				1
				SUR_LEFT			2
				SUR_RIGHT			3
				CH_FC				4
				CH_LFE				5
				=>
				Front Left - FL			0
				Front Right - FR		1
				Front Center - FC		2
				Low Frequency - LF		3
				Back Left - BL			4
				Back Right - BR			5
				*/
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer[1] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel1), -32768), 32767));

				dmaOutputBuffer[4] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel2), -32768), 32767));
				dmaOutputBuffer[5] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel3), -32768), 32767));

				dmaOutputBuffer[2] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel4), -32768), 32767));
				dmaOutputBuffer[3] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel5), -32768), 32767));
				dmaOutputBuffer += 6;
				// next sample
				inputChannel0++;
				inputChannel1++;
				inputChannel2++;
				inputChannel3++;
				inputChannel4++;
				inputChannel5++;
			}
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 6 * sizeof(sint16)); // 6ch output
		}
		else if (__AXMode[AX_DEV_TV] == AX_MODE_STEREO)
		{
			sint32* inputChannel0 = __AXTVBuffer48.GetPtr() + numSamples * 0;
			sint32* inputChannel1 = __AXTVBuffer48.GetPtr() + numSamples * 1;
			sint16* dmaOutputBuffer = __AXTVDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer[1] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel1), -32768), 32767));
				dmaOutputBuffer += 2;
				// next sample
				inputChannel0++;
				inputChannel1++;
			}
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 2 * sizeof(sint16)); // 2ch output
		}
		else if (__AXMode[AX_DEV_TV] == AX_MODE_MONO)
		{
			sint32* inputChannel0 = __AXTVBuffer48.GetPtr() + numSamples * 0;
			sint16* dmaOutputBuffer = __AXTVDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer++;
				// next sample
				inputChannel0++;
			}
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 1 * sizeof(sint16)); // 1ch (output as stereo)
		}
		else
			assert_dbg();
	}

	void AIInitDRCDMA(sint16* sampleData, sint32 size)
	{
		sint32 sampleCount = size / sizeof(sint16); // sample count in total (summed up for all channels)

		if (sndGeneric.initParam.frameLength != 0)
		{
			cemu_assert(false);
		}

		std::shared_lock lock(g_audioMutex);

		const uint32 channels = g_padAudio ? g_padAudio->GetChannels() : AX_DRC_CHANNEL_COUNT;
		sint16* outputChannel = tempDRCChannelData + AX_SAMPLES_PER_3MS_48KHZ * tempDRCAudioBlockCounter * channels;
		for (sint32 i = 0; i < sampleCount; ++i)
		{
			outputChannel[i] = _swapEndianS16(sampleData[i]);
		}

		tempDRCAudioBlockCounter++;
		if (tempDRCAudioBlockCounter == AX_FRAMES_PER_GROUP)
		{
			if (g_padAudio)
			{
				const bool accepted = g_padAudio->FeedBlock(tempDRCChannelData);
				if (IsMN9AudioTraceTarget())
					(accepted ? s_mn9DRCFeedAccepted : s_mn9DRCFeedRejected).fetch_add(1, std::memory_order_relaxed);
			}

			tempDRCAudioBlockCounter = 0;
		}
	}

	void AXOut_SubmitDRCFrame(sint32 frameIndex)
	{
		sint32 numSamples = AIGetSamplesPerChannel(AX_DEV_DRC);
		if (__AXMode[AX_DEV_DRC] == AX_MODE_6CH)
		{
			sint32* inputChannel0 = __AXDRCBuffer48.GetPtr() + numSamples * 0;
			sint32* inputChannel1 = __AXDRCBuffer48.GetPtr() + numSamples * 1;
			sint32* inputChannel2 = __AXDRCBuffer48.GetPtr() + numSamples * 2;
			sint32* inputChannel3 = __AXDRCBuffer48.GetPtr() + numSamples * 3;
			sint16* dmaOutputBuffer = AIGetCurrentDMABuffer(AX_DEV_DRC);
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer[1] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel1), -32768), 32767));

				dmaOutputBuffer[4] = 0;
				dmaOutputBuffer[5] = 0;

				dmaOutputBuffer[2] = 0;
				dmaOutputBuffer[3] = 0;
				dmaOutputBuffer += 6;
				// next sample
				inputChannel0++;
				inputChannel1++;
				inputChannel2++;
				inputChannel3++;
			}
			AIInitDRCDMA(__AXDRCDMABuffers[frameIndex], numSamples * 6 * sizeof(sint16)); // 6ch output
		}
		else if (__AXMode[AX_DEV_DRC] == AX_MODE_STEREO)
		{
			sint32* inputChannel0 = __AXDRCBuffer48.GetPtr() + numSamples * 0;
			sint32* inputChannel1 = __AXDRCBuffer48.GetPtr() + numSamples * 1;
			sint16* dmaOutputBuffer = __AXDRCDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer[1] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel1), -32768), 32767));
				dmaOutputBuffer += 2;
				// next sample
				inputChannel0++;
				inputChannel1++;
			}

			AIInitDRCDMA(__AXDRCDMABuffers[frameIndex], numSamples * 2 * sizeof(sint16)); // 2ch output
		}
		else if (__AXMode[AX_DEV_DRC] == AX_MODE_MONO)
		{
			sint32* inputChannel0 = __AXDRCBuffer48.GetPtr() + numSamples * 0;
			sint16* dmaOutputBuffer = __AXDRCDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				// write mono input as stereo output
				dmaOutputBuffer[1] = dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer += 2;
				// next sample
				inputChannel0++;
			}
			AIInitDRCDMA(__AXDRCDMABuffers[frameIndex], numSamples * 2 * sizeof(sint16)); // 1ch (output as stereo)
		}
		else
			assert_dbg();
	}

	/* AX output */

	uint32 numQueuedFramesSndGeneric = 0;

	void AXOut_init()
	{

		numQueuedFramesSndGeneric = 0;

		std::unique_lock lock(g_audioMutex);
		if (!g_tvAudio)
		{
			try
			{
				g_tvAudio = IAudioAPI::CreateDeviceFromConfig(IAudioAPI::AudioType::TV, 48000, snd_core::AX_SAMPLES_PER_3MS_48KHZ * AX_FRAMES_PER_GROUP, 16);
			}
			catch (std::runtime_error& ex)
			{
				cemuLog_log(LogType::Force, "can't initialize tv audio: {}", ex.what());
			}
		}

		g_padVolume = GetConfig().pad_volume;
		if (!g_padAudio)
		{
			try
			{
				g_padAudio = IAudioAPI::CreateDeviceFromConfig(IAudioAPI::AudioType::Gamepad, 48000, snd_core::AX_SAMPLES_PER_3MS_48KHZ * AX_FRAMES_PER_GROUP, 16);
			}
			catch (std::runtime_error& ex)
			{
				cemuLog_log(LogType::Force, "can't initialize pad audio: {}", ex.what());
			}
		}
	}

	void AXOut_reset()
	{
		std::unique_lock lock(g_audioMutex);
		if (g_tvAudio)
		{
			g_tvAudio->Stop();
			g_tvAudio.reset();
		}
		if (g_padAudio)
		{
			g_padAudio->Stop();
			g_padAudio.reset();
		}
		if (g_portalAudio)
		{
			g_portalAudio->Stop();
			g_portalAudio.reset();
		}
	}

	void AXOut_updateDevicePlayState(bool isPlaying)
	{
		std::shared_lock lock(g_audioMutex);
		if (isPlaying && coreinit::NativeHomeSession::KeepsGameAudioTimeline())
		{
			// A settings change can replace a sink since the scheduler's mute check.
			const bool muted = coreinit::NativeHomeSession::IsGameAudioSuspended();
			if (g_tvAudio) g_tvAudio->SetOutputMuted(muted);
			if (g_padAudio) g_padAudio->SetOutputMuted(muted);
			if (g_portalAudio) g_portalAudio->SetOutputMuted(muted);
		}
		if (g_tvAudio)
		{
			if (isPlaying)
				g_tvAudio->Play();
			else
				g_tvAudio->Stop();
		}

		if (g_padAudio)
		{
			if (isPlaying)
				g_padAudio->Play();
			else
				g_padAudio->Stop();
		}

		if (g_portalAudio)
		{
			if (isPlaying)
				g_portalAudio->Play();
			else
				g_portalAudio->Stop();
		}
	}

	// called periodically to check for AX updates
	void AXOut_update()
	{
		constexpr static auto kTimeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(((IAudioAPI::kBlockCount * 3) / 4) * (AX_FRAMES_PER_GROUP * 3)));
		constexpr static auto kWaitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(3));
		constexpr static auto kWaitDurationFast = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::microseconds(2900));
		constexpr static auto kWaitDurationMinimum = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::microseconds(1700));

		static auto s_ax_interval_timer = now_cached() - kWaitDuration;
		static auto s_last_check = now_cached();
		static bool s_homeAudioPaused = false;
		const auto now = now_cached();
		const bool keepTimeline = coreinit::NativeHomeSession::KeepsGameAudioTimeline();
		if (keepTimeline)
		{
			static bool s_timelineMuted = false;
			std::shared_lock lock(g_audioMutex);
			const bool muted = keepTimeline && coreinit::NativeHomeSession::IsGameAudioSuspended();
			// Mute only this process's sinks. Native HOME/manual keep their own audio.
			if (g_tvAudio) g_tvAudio->SetOutputMuted(muted);
			if (g_padAudio) g_padAudio->SetOutputMuted(muted);
			if (g_portalAudio) g_portalAudio->SetOutputMuted(muted);
			if (muted != s_timelineMuted)
			{
				cemuLog_log(LogType::Force, "[COMPAT:NBA-HOME:AUDIO] output={}", muted ? "muted" : "active");
				s_timelineMuted = muted;
			}
		}
		if (!NbaHomeAudio::MixerMayRun(coreinit::NativeHomeSession::IsGameAudioSuspended(),
			coreinit::NativeHomeSession::IsGameAudioQuiescent(), keepTimeline))
		{
			if (!s_homeAudioPaused)
			{
				if (!keepTimeline) AXOut_updateDevicePlayState(false);
				else cemuLog_log(LogType::Force, "[COMPAT:NBA-HOME:AUDIO] release complete; guest_AX_timeline=held");
			}
			s_homeAudioPaused = true;
			s_ax_interval_timer = s_last_check = now;
			return;
		}
		if (s_homeAudioPaused)
		{
			// Suspended wall time is not audio debt. Resume the same voices
			// without rushing through the time spent inside HOME/manual.
			s_homeAudioPaused = false;
			s_ax_interval_timer = s_last_check = now - kWaitDuration;
			if (keepTimeline) cemuLog_log(LogType::Force, "[COMPAT:NBA-HOME:AUDIO] guest_AX_timeline=resumed cadence=rebased");
		}

		// if we haven't buffered any blocks, we will wait less time than usual
		bool additional_blocks_required = false;
		{
			const std::shared_lock lock(g_audioMutex, std::try_to_lock);
			if (lock)
				additional_blocks_required = (g_tvAudio && g_tvAudio->NeedAdditionalBlocks()) || (g_padAudio && g_padAudio->NeedAdditionalBlocks());
		}

		const auto wait_duration = additional_blocks_required ? kWaitDurationFast : kWaitDuration;
		const bool enableAudioCatchUp = WheelAudioTiming::Enabled(CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) ||
			IsMN9AudioTraceTarget() || IsJD2014AudioCatchUpTarget() || IsJD2015AudioCatchUpTarget() || IsJD2016AudioCatchUpTarget() || IsJD2017AudioCatchUpTarget() || IsJD2018AudioCatchUpTarget() || IsJD2019AudioCatchUpTarget() || IsMarioParty10AudioCatchUpTarget() || IsAvengersBattleForEarthAudioCatchUpTarget() || IsNbaAudioCatchUpTarget() || IsNinjaAudioCatchUpTarget() || IsRabbidsAudioCatchUpTarget() || IsRaymanAudioCatchUpTarget() || IsSpongeAudioCatchUpTarget() || IsDigAudioCatchUpTarget() || IsTerrariaAudioCatchUpTarget();

		// s_ax_interval_timer increases by the wait period
		// it can lag behind by multiple periods (up to kTimeout) if there is minor stutter in the CPU thread
		// s_last_check is always set to the timestamp at the time of firing
		// it's used to enforce the minimum wait delay (we want to avoid calling AX update in quick succession because other threads may need to do work first) 

		const auto diff = (now - s_ax_interval_timer);

		if (diff < wait_duration)
			return;

		// handle minimum wait time (1.7MS)
		if ((now - s_last_check) < kWaitDurationMinimum)
			return;
		s_last_check = now;

		// The gated titles can keep the main core busy long enough that this update is
		// called less often than the 3 ms AX cadence. Preserve a bounded amount of
		// elapsed work instead of silently reducing the guest audio sample rate.
		const bool timeoutResync = diff >= kTimeout;
		uint32 elapsedFrameCount = 1;
		if (enableAudioCatchUp)
		{
			constexpr uint32 kMaxCatchUpFrames = 8;
			elapsedFrameCount = std::clamp<uint32>((uint32)(diff / wait_duration), 1, kMaxCatchUpFrames);
			if (timeoutResync)
				s_ax_interval_timer = now - wait_duration * elapsedFrameCount;
		}
		else if (timeoutResync)
			s_ax_interval_timer = (now - wait_duration);
		else
			s_ax_interval_timer += wait_duration;

		const bool sndInitialized = snd_core::isInitialized();
		uint32 queuedFrameCount = 0;
		bool waitingForMixer = false;
		if (sndInitialized)
		{
			const uint32 processedFrames = snd_core::getNumProcessedFrames();
			const uint32 outstandingFrames = numQueuedFramesSndGeneric - processedFrames;
			uint32 framesToQueue = 0;
			if (enableAudioCatchUp)
			{
				constexpr uint32 kMaxOutstandingFrames = 8;
				if (outstandingFrames < kMaxOutstandingFrames)
					framesToQueue = std::min(elapsedFrameCount, kMaxOutstandingFrames - outstandingFrames);
			}
			else if (outstandingFrames == 0)
				framesToQueue = 1;

			for (uint32 frame = 0; frame < framesToQueue; ++frame)
			{
				AXOut_updateDevicePlayState(true);
				snd_core::AXIst_QueueFrame();
				numQueuedFramesSndGeneric++;
				queuedFrameCount++;
			}
			waitingForMixer = framesToQueue == 0 && outstandingFrames != 0;

			if (enableAudioCatchUp)
				s_ax_interval_timer += wait_duration * queuedFrameCount;
		}

		static auto s_traceWindowStart = now;
		static uint32 s_traceStartQueued = numQueuedFramesSndGeneric;
		static uint32 s_traceStartProcessed = snd_core::getNumProcessedFrames();
		static uint64 s_traceChecks = 0;
		static uint64 s_traceRefillNeeded = 0;
		static uint64 s_traceQueued = 0;
		static uint64 s_traceCatchUpQueued = 0;
		static uint32 s_traceMaxElapsedFrames = 0;
		static uint64 s_traceWaiting = 0;
		static uint64 s_traceTimeoutResyncs = 0;
		const bool traceNbaResume = keepTimeline && NbaHomeAudio::TraceMovie();
		if (!IsMN9AudioTraceTarget() && !traceNbaResume)
		{
			s_traceWindowStart = now;
			s_traceStartQueued = numQueuedFramesSndGeneric;
			s_traceStartProcessed = snd_core::getNumProcessedFrames();
			s_traceChecks = 0;
			s_traceRefillNeeded = 0;
			s_traceQueued = 0;
			s_traceCatchUpQueued = 0;
			s_traceMaxElapsedFrames = 0;
			s_traceWaiting = 0;
			s_traceTimeoutResyncs = 0;
			s_mn9TVFeedAccepted.store(0, std::memory_order_relaxed);
			s_mn9TVFeedRejected.store(0, std::memory_order_relaxed);
			s_mn9DRCFeedAccepted.store(0, std::memory_order_relaxed);
			s_mn9DRCFeedRejected.store(0, std::memory_order_relaxed);
			return;
		}

		s_traceChecks++;
		s_traceRefillNeeded += additional_blocks_required;
		s_traceQueued += queuedFrameCount;
		s_traceCatchUpQueued += queuedFrameCount > 1 ? queuedFrameCount - 1 : 0;
		s_traceMaxElapsedFrames = std::max(s_traceMaxElapsedFrames, elapsedFrameCount);
		s_traceWaiting += waitingForMixer;
		s_traceTimeoutResyncs += timeoutResync;
		const double traceIntervalMs = std::chrono::duration<double, std::milli>(now - s_traceWindowStart).count();
		if (traceIntervalMs >= 1000.0)
		{
			if (traceNbaResume)
			{
				const auto movie = NbaHomeMovieTiming::counters.Take();
				cemuLog_log(LogType::Force,
					"[COMPAT:NBA-HOME:MOVIE] interval_ms={:.1f} calls={} fast={} native={} parity={} avg_ms={:.3f} max_ms={:.3f} fallback_pitch={} fallback_gqr={:08x} swaps={} avg_swap_ms={:.3f} max_swap_ms={:.3f} focused_pid={} owner_pid={}",
					traceIntervalMs, movie.calls, movie.fast, movie.native, movie.parity,
					movie.calls ? movie.totalNs / (double)movie.calls / 1.0e6 : 0.0, movie.maxNs / 1.0e6,
					movie.fallbackPitch, movie.fallbackGqr, movie.swaps,
					movie.swaps ? movie.swapNs / (double)movie.swaps / 1.0e6 : 0.0, movie.maxSwapNs / 1.0e6,
					movie.focusedPid, movie.ownerPid);
				cemuLog_log(LogType::Force,
					"[COMPAT:NBA-HOME:NATIVE] interval_ms={:.1f} rows={} sum_row_ms={:.3f} max_row_ms={:.3f} converted={} native_index_first={} native_index_last={}",
					traceIntervalMs, movie.rows, movie.rowNs / 1.0e6, movie.maxRowNs / 1.0e6,
					movie.movieSamples, movie.nativeIndexFirst, movie.nativeIndexLast);
			}
			const uint32 processedNow = snd_core::getNumProcessedFrames();
			const uint32 queuedDelta = numQueuedFramesSndGeneric - s_traceStartQueued;
			const uint32 processedDelta = processedNow - s_traceStartProcessed;
			cemuLog_log(LogType::Force,
				"[COMPAT:{}:AUDIO] layer=scheduler interval_ms={:.1f} checks={} refill_needed={} queue_calls={} catchup_frames={} max_elapsed_frames={} queued={} processed={} outstanding={} mixer_wait_checks={} timeout_resyncs={} tv_feed_ok={} tv_feed_drop={} drc_feed_ok={} drc_feed_drop={}",
				traceNbaResume ? "NBA-HOME" : "MN9", traceIntervalMs, s_traceChecks, s_traceRefillNeeded, s_traceQueued,
				s_traceCatchUpQueued, s_traceMaxElapsedFrames, queuedDelta, processedDelta,
				(sint64)numQueuedFramesSndGeneric - (sint64)processedNow, s_traceWaiting, s_traceTimeoutResyncs,
				s_mn9TVFeedAccepted.exchange(0, std::memory_order_relaxed),
				s_mn9TVFeedRejected.exchange(0, std::memory_order_relaxed),
				s_mn9DRCFeedAccepted.exchange(0, std::memory_order_relaxed),
				s_mn9DRCFeedRejected.exchange(0, std::memory_order_relaxed));
			s_traceWindowStart = now;
			s_traceStartQueued = numQueuedFramesSndGeneric;
			s_traceStartProcessed = processedNow;
			s_traceChecks = 0;
			s_traceRefillNeeded = 0;
			s_traceQueued = 0;
			s_traceCatchUpQueued = 0;
			s_traceMaxElapsedFrames = 0;
			s_traceWaiting = 0;
			s_traceTimeoutResyncs = 0;
		}
	}

}
