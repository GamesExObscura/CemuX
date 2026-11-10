#pragma once
#include <cstdint>
#include <string_view>
#include "HomeControllerBridge.h"

namespace coreinit::NativeHomeSession
{
	bool IsConfigured();
	bool IsChild();
	bool RequestInputSettings();
	bool SetRemoteDataFormat(std::uint32_t channel, std::uint32_t format);
	bool IsGameAudioSuspended();
	bool KeepsGameAudioTimeline();
	bool IsGameAudioQuiescent();
	bool IsHelperStartup();
	std::uint64_t GetHelperTimerFrequency();
	void SetCloseToWiiUMenu(bool enabled);
	void Initialize();
	void Shutdown();
	void Poll();
	void NotifyScanout();
	bool ReleaseForeground(std::uint32_t core);
	bool RequestReturn(bool closeSoftware);
	bool RequestRootSwitch();
	std::uint64_t MainApplicationTitle();
	bool GetCaller(std::uint32_t& caller);
	bool ReadMenuButtons(std::uint32_t& hold, std::uint32_t& pressed, std::uint32_t& released);
	bool GetGamePadRumble(bool& enabled);
	std::uint8_t GetGamePadBatteryLevel();
	bool SetGamePadRumble(bool enabled);
	bool RequestManual();
	bool BindManualMetadata(std::string_view source, std::string_view destination);
	bool UnbindManualMetadata(std::string_view path);
	void SystemMessageReceived(std::uint32_t id);
	bool GetRemoteStatus(std::uint32_t channel, HomeRemoteStatus& status);
	bool SendRemoteCommand(const RemoteCommand& command, std::uint32_t* ticket = nullptr);
	bool RemoteSpeakerResult(std::uint32_t channel,std::uint32_t ticket,std::int32_t& result);
	bool CanSendRemoteStream();
	bool GetRemoteSettings(std::uint32_t& volume, bool& motors, std::uint32_t& generation, bool& syncing, std::uint32_t& count);
}
