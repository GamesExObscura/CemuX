#include "Cafe/OS/common/OSCommon.h"
#include "HomeSession.h"
#include "HomeSessionProtocol.h"
#include "HomeMenuProtocol.h"
#include "coreinit_MessageQueue.h"
#include "coreinit_Misc.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/NbaHomeAudio.h"
#include "Cafe/NbaHomeFocus.h"
#include "Cafe/NbaMovieResumeTime.h"
#include "coreinit_Time.h"
#include "Cafe/OS/libs/proc_ui/proc_ui.h"
#include "Cafe/OS/libs/sysapp/sysapp.h"
#include "config/ActiveSettings.h"
#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "input/emulated/WiimoteController.h"
#include "input/api/Controller.h"
#include "input/api/Wiimote/NativeWiimoteController.h"
#include <unordered_map>
#include <bit>
#include "WindowSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/TitleList/TitleInfo.h"

#if BOOST_OS_WINDOWS
#include <wx/app.h>
#include <wx/window.h>
#include <wx/toplevel.h>
#include "gui/wxgui/MainWindow.h"
#include <fmt/xchar.h>
#include <random>

namespace coreinit
{
	uint32 __LaunchByTitleId(uint64 titleId, uint32 argc, MEMPTR<char>* argv);
}

namespace coreinit::NativeHomeSession
{
	namespace
	{
		enum class Phase { Idle, ReleasingGame, Home, Resume, Close, OpenManual, ReturnHome };
		enum Action : std::uint32_t { None, Resume, CloseSoftware, OpenManual, ReturnHome };

		SharedSession* s_shared{};
		HANDLE s_mapping{}, s_peer{};
		std::wstring s_mappingName;
		Phase s_phase = Phase::Idle;
		std::atomic_bool s_gameAudioSuspended{false};
		std::atomic_bool s_gameAudioQuiescent{false};
		ReleaseBarrier s_release;
		HomeMenuProtocol::ActivationInput s_activation;
		HomeMenuProtocol::ActivationInput s_resumeActivation;
		HomeMenuProtocol::ActivationInput s_deniedActivation;
		std::unique_ptr<TitleInfo> s_retainedMetadata;
		ButtonEdges s_edges;
		ButtonHandoffGate s_componentInput;
		ButtonHandoffGate s_padComponentInput;
		std::array<std::shared_ptr<WPADController>,7> s_remoteInputMirrors;
		std::atomic_bool s_inputSettingsPosted{false};
		std::atomic<std::uint32_t> s_requestedAction{};
		std::atomic<std::uint32_t> s_seenActivation{};
		std::atomic<std::uint32_t> s_caller{};
		std::atomic<bool> s_closeToWiiUMenu{};
		bool s_child{}, s_parent{}, s_ignoreHome{}, s_closePosted{}, s_preloadTried{};
		std::uint32_t s_seenResume{};
		bool s_manual{};
		bool s_manualMetadataBound{};
		std::atomic<bool> s_manualForeground{};
		bool s_manualShown{};
		std::atomic<bool> s_trackFrames{}, s_componentRetiring{};
		std::atomic<std::uint64_t> s_nativeFrames{};
		std::atomic<std::uint32_t> s_shownActivation{};
		std::uint64_t s_frameBaseline{};
		bool s_showPending{}, s_handoffPending{};
		struct PreparedApplet
		{
			SharedSession* shared{};
			HANDLE mapping{}, peer{};
			std::wstring mappingName;
			bool attempted{};
		} s_prepared;
		std::uint32_t s_rumbleRevision{};
		std::unordered_map<std::string, float> s_rumbleLevels;
		std::uint32_t s_seenLayout{};
		std::chrono::steady_clock::time_point s_nextLayoutCheck;
		std::chrono::steady_clock::time_point s_phaseStart;
		std::mutex s_remoteCommandMutex;
		bool s_syncBusy{};
		std::uint32_t s_syncMask{}, s_syncGeneration{}, s_syncCount{};
		std::chrono::steady_clock::time_point s_syncDeadline;
		std::chrono::steady_clock::time_point s_nextControllerPublish;
		std::array<HomeRemoteStatus,7> s_remoteCache{};
		std::array<std::uint32_t,5> s_remoteSettings{64,1,0,0,0};
		void UpdateControllers();
		std::array<bool,7> s_speakerControlActive{};
		std::array<std::uint32_t,7> s_speakerControlTicket{};

		std::wstring Environment(const wchar_t* key)
		{
			const auto length = GetEnvironmentVariableW(key, nullptr, 0);
			if (!length || length > 32768) return {};
			std::wstring value(length, L'\0');
			const auto copied = GetEnvironmentVariableW(key, value.data(), length);
			if (!copied || copied >= length) return {};
			value.resize(copied);
			return value;
		}

		SurfaceArea CaptureSurface(const WindowSystem::WindowHandleInfo& canvas, const WindowSystem::WindowHandleInfo& frame)
		{
			const auto surface = static_cast<HWND>(canvas.surface), owner = static_cast<HWND>(frame.surface);
			DWORD surfacePid{}, ownerPid{};
			GetWindowThreadProcessId(surface, &surfacePid); GetWindowThreadProcessId(owner, &ownerPid);
			if (surfacePid != GetCurrentProcessId() || ownerPid != surfacePid || !IsWindowVisible(owner) || IsIconic(owner)) return {};
			RECT rect{}; POINT origin{};
			if (!GetClientRect(surface, &rect) || !ClientToScreen(surface, &origin)) return {};
			return {reinterpret_cast<std::uintptr_t>(owner), origin.x, origin.y, rect.right, rect.bottom};
		}

		void CaptureLayout()
		{
			const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
			const auto& info = WindowSystem::GetWindowInfo();
			const SurfaceLayout layout{CaptureSurface(info.canvas_main, info.window_main), CaptureSurface(info.canvas_pad, info.window_pad)};
			if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
			SurfaceLayout old; std::uint32_t sequence{};
			if (!ReadLayout(*s_shared, old, sequence) || old != layout) PublishLayout(*s_shared, layout);
		}

		void ShowApplet(bool visible, bool activate = true)
		{
			if (!wxTheApp) return;
			SurfaceLayout layout; std::uint32_t sequence{};
			if (visible && (!s_shared || !ReadLayout(*s_shared, layout, sequence))) return;
			const auto ownerPid = s_shared ? s_shared->ownerPid : 0;
			const auto activationTick = s_shared ? std::atomic_ref(s_shared->activationTick).load() : 0;
			const auto activation = s_shared ? Load(s_shared->activation) : 0;
			wxTheApp->CallAfter([visible, activate, layout, ownerPid, activationTick, activation] {
				if (visible && s_componentRetiring.load()) return;
				auto* main = dynamic_cast<MainWindow*>(wxTheApp->GetTopWindow());
				if (!main) return;
				if (visible) main->SetMenuVisible(false);
				auto place = [&](wxTopLevelWindow* window, const SurfaceArea& area) {
					if (!window) return;
					const auto owner = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(area.owner));
					DWORD pid{}; GetWindowThreadProcessId(owner, &pid);
					if (!visible || !area.Valid() || pid != ownerPid) { window->Hide(); return; }
					if (window->IsMaximized()) window->Maximize(false);
					const auto handle = static_cast<HWND>(window->GetHandle());
					SetWindowLongPtrW(handle, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
					const auto style = GetWindowLongPtrW(handle, GWL_STYLE);
					SetWindowLongPtrW(handle, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
					const auto extended = GetWindowLongPtrW(handle, GWL_EXSTYLE);
					SetWindowLongPtrW(handle, GWL_EXSTYLE, (extended & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
					const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
					SetWindowPos(handle, HWND_TOP, area.x, area.y, area.width, area.height,
						SWP_FRAMECHANGED | SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE));
					if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
				};
				place(main, layout.tv); place(main->GetPadView(), layout.pad);
				if (visible && IsWindowVisible(static_cast<HWND>(main->GetHandle())) &&
					(!layout.pad.Valid() || (main->GetPadView() && IsWindowVisible(static_cast<HWND>(main->GetPadView()->GetHandle())))))
					s_shownActivation = activation;
				if (visible && activate)
				{
					main->Raise(); main->SetFocus();
					cemuLog_log(LogType::Force, "HOME timing: native windows shown {} ms after HOME request (not a presented-frame measurement)", GetTickCount64() - activationTick);
				}
			});
		}

		void CloseApplet()
		{
			if (s_closePosted || !wxTheApp) return;
			s_closePosted = true;
			wxTheApp->CallAfter([] {
				if (auto* top = wxDynamicCast(wxTheApp->GetTopWindow(), wxTopLevelWindow)) top->Close();
			});
		}

		void RestoreGameWindow(bool activateForeground)
		{
			if (!wxTheApp) return;
			wxTheApp->CallAfter([activateForeground] {
				auto* top = wxTheApp->GetTopWindow();
				if (!top) return;
				top->Raise();
				struct OwnerWindow
				{
					HWND handle;
					HWND Window() const { return handle; }
					bool Valid(HWND window) const { return IsWindow(window) != FALSE; }
					bool Visible(HWND window) const { return IsWindowVisible(window) != FALSE; }
					DWORD Owner(HWND window) const { DWORD pid{}; GetWindowThreadProcessId(window, &pid); return pid; }
					DWORD CurrentProcess() const { return GetCurrentProcessId(); }
					bool Activate(HWND window) const { return SetForegroundWindow(window) != FALSE; }
				} window{static_cast<HWND>(top->GetHandle())};
				const bool requested = activateForeground && KeepsGameAudioTimeline();
				const bool activated = NbaHomeFocus::Restore(window, requested);
				top->SetFocus();
				if (requested)
				{
					DWORD focusedPid{}; GetWindowThreadProcessId(GetForegroundWindow(), &focusedPid);
					cemuLog_log(LogType::Force, "[COMPAT:NBA-HOME:FOCUS] explicit_resume=1 api_result={} focused_pid={} owner_pid={}",
						activated, focusedPid, GetCurrentProcessId());
				}
			});
		}

		bool Queue(SysMessageId id)
		{
			OSMessage message{};
			message.data0 = stdx::to_underlying(id);
			message.data1 = id == SysMessageId::MsgAcquireForeground ? 1 : 0;
			message.data2 = id == SysMessageId::MsgAcquireForeground ? 1 : 0;
			return OSSendMessage(OSGetSystemMessageQueue(), &message, 0);
		}

		std::uint32_t ReadButtons(bool gamepadOnly = false)
		{
			std::uint32_t buttons = 0;
			auto read = [&](const auto& controller) {
				if (!controller) return;
				controller->controllers_update_states();
				if (controller->is_a_down()) buttons |= 0x8000;
				if (controller->is_b_down()) buttons |= 0x4000;
				if (controller->is_start_down()) buttons |= 8;
				if (controller->is_home_down()) buttons |= 2;
				const auto axis = controller->get_axis();
				if (controller->is_up_down() || axis.y > 0.5f) buttons |= 0x0200;
				if (controller->is_down_down() || axis.y < -0.5f) buttons |= 0x0100;
				if (controller->is_left_down() || axis.x < -0.5f) buttons |= 0x0800;
				if (controller->is_right_down() || axis.x > 0.5f) buttons |= 0x0400;
			};
			const auto& input = InputManager::instance();
			for (size_t i = 0; i < InputManager::kMaxVPADControllers; ++i) read(input.get_vpad_controller(i));
			if (!gamepadOnly) for (size_t i = 0; i < InputManager::kMaxWPADControllers; ++i) read(input.get_wpad_controller(i));
			DWORD focusedPid{};
			GetWindowThreadProcessId(GetForegroundWindow(), &focusedPid);
			if (NbaHomeAudio::TraceResume()) NbaHomeMovieTiming::counters.Focus(focusedPid, GetCurrentProcessId());
			if ((focusedPid == GetCurrentProcessId() || (s_peer && focusedPid == GetProcessId(s_peer))) && (GetAsyncKeyState('H') & 0x8000))
				buttons |= 2; // Private-prototype fallback, never a global desktop hotkey.
			return buttons;
		}

		void PublishPadRumble()
		{
			bool enabled = false;
			if (const auto pad = InputManager::instance().get_vpad_controller(0))
				for (const auto& device : pad->get_controllers()) enabled |= device->get_settings().rumble > 0;
			Store(s_shared->padRumble, enabled);
		}

		void ApplyPadRumble()
		{
			const auto revision = Load(s_shared->padRumbleRevision);
			if (revision == s_rumbleRevision) return;
			s_rumbleRevision = revision;
			const bool enabled = Load(s_shared->padRumble) != 0;
			if (const auto pad = InputManager::instance().get_vpad_controller(0))
			{
				for (const auto& device : pad->get_controllers())
				{
					const auto current = device->get_settings().rumble;
					auto& remembered = s_rumbleLevels[std::string(device->api_name()) + ":" + device->uuid()];
					if (current > 0) remembered = current;
					device->set_rumble(enabled ? (remembered > 0 ? remembered : 1.0f) : 0.0f);
					if (!enabled) device->stop_rumble();
				}
				try
				{
					const bool saved = InputManager::instance().save(pad->player_index());
					cemuLog_log(LogType::Force, "HOME controller settings: GamePad rumble={} applied to active Cemu profile; standard profile save={}", enabled, saved);
				}
				catch (const std::exception& error)
				{
					cemuLog_log(LogType::Force, "HOME controller settings: rumble changed in memory but profile could not be saved: {}", error.what());
				}
			}
		}

		fs::path ChildExecutable()
		{
			const auto bundled = ActiveSettings::GetExecutablePath().parent_path() / (s_manual ? "native-manual" : "native-home") / "Cemu_release.exe";
			std::error_code error;
			if (fs::is_regular_file(bundled, error)) return bundled;
			return ActiveSettings::GetExecutablePath().parent_path().parent_path() / (s_manual ? "native-manual" : "native-home") / "Cemu_release.exe";
		}

		fs::path HomeRpx()
		{
			const auto region = (_SYSGetSystemApplicationTitleId(0) >> 8) & 0xff;
			const auto low = 0x1001000au + (region <= 2 ? static_cast<std::uint32_t>(region) << 8 : 0x100);
			return ActiveSettings::GetMlcPath() / "sys/title/00050030" / fmt::format("{:08x}", low) / "code/hbm.rpx";
		}

		bool CreateSession()
		{
			const auto executable = ChildExecutable();
			const auto home = HomeRpx();
			std::error_code executableError, homeError;
			const bool executableExists = fs::is_regular_file(executable, executableError);
			const bool homeExists = fs::is_regular_file(home, homeError);
			if (!executableExists || !homeExists)
			{
				cemuLog_log(LogType::Force, "HOME prototype: dependency check failed; child={} exists={} error={}; HOME={} exists={} error={}; game unchanged",
					_pathToUtf8(executable), executableExists, executableError.value(), _pathToUtf8(home), homeExists, homeError.value());
				return false;
			}
			if (s_shared) return true;
			std::random_device random;
			s_mappingName = fmt::format(L"Local\\CemuNativeHome-{}-{:08x}{:08x}{:08x}{:08x}", GetCurrentProcessId(), random(), random(), random(), random());
			s_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedSession), s_mappingName.c_str());
			if (!s_mapping) return false;
			if (GetLastError() == ERROR_ALREADY_EXISTS)
			{
				CloseHandle(s_mapping); s_mapping = nullptr; return false;
			}
			s_shared = static_cast<SharedSession*>(MapViewOfFile(s_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedSession)));
			if (!s_shared)
			{
				CloseHandle(s_mapping); s_mapping = nullptr; return false;
			}
			new (s_shared) SharedSession{};
			s_shared->ownerPid = GetCurrentProcessId();
			s_shared->gameTitle = CafeSystem::GetForegroundTitleId();
			s_shared->timerFrequency = PPCTimer_getFrequency();
			UpdateControllers();
			return true;
		}

		fs::path ManualRpx()
		{
			const auto region = (_SYSGetSystemApplicationTitleId(0) >> 8) & 0xff;
			const auto low = 0x1001700au + (region <= 2 ? static_cast<std::uint32_t>(region) << 8 : 0x100);
			return ActiveSettings::GetMlcPath() / "sys/title/00050030" / fmt::format("{:08x}", low) / "code/recipe.rpx";
		}

		bool BindRetainedMetadata()
		{
			const auto path = Environment(L"CEMU_HOME_GAME_PATH");
			if (path.empty()) return false;
			const auto subpath = _pathToUtf8(fs::path(Environment(L"CEMU_HOME_GAME_SUBPATH")));
			auto title = subpath.empty() ? std::make_unique<TitleInfo>(fs::path(path)) :
				std::make_unique<TitleInfo>(fs::path(path), subpath);
			if (!title->IsValid() || title->GetAppTitleId() != s_shared->gameTitle)
			{
				cemuLog_log(LogType::Force, "HOME metadata: rejected invalid or mismatched retained title; no substitute metadata used");
				return false;
			}
			const auto destination = CafeSystem::GetMlcStoragePath(s_shared->gameTitle) + "/meta";
			if (!title->Mount(destination, "meta", FSC_PRIORITY_BASE)) return false;
			if (s_retainedMetadata) s_retainedMetadata->UnmountAll();
			s_retainedMetadata = std::move(title);
			cemuLog_log(LogType::Force, "HOME metadata: bound original retained-game metadata at {}", destination);
			return true;
		}

		bool LaunchApplet()
		{
			if (s_peer && WaitForSingleObject(s_peer, 0) == WAIT_TIMEOUT) return true;
			if (s_peer) { CloseHandle(s_peer); s_peer = nullptr; }
			const auto executable = ChildExecutable();
			// Preserve the previous helper's ordinary log before Cemu truncates it.
			std::error_code archiveError;
			const auto logs = ActiveSettings::GetUserDataPath("native-home-logs");
			fs::create_directories(logs, archiveError);
			const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
			if (!archiveError) fs::copy_file(executable.parent_path() / "portable/log.txt",
				logs / fmt::format("home-{:016x}-{}.log", s_shared->gameTitle, stamp), fs::copy_options::none, archiveError);
			const auto rpx = s_manual ? ManualRpx() : HomeRpx();
			auto command = fmt::format(L"\"{}\" -g \"{}\" -m \"{}\" -a {:08x}", executable.wstring(), rpx.wstring(), ActiveSettings::GetMlcPath().wstring(), ActiveSettings::GetPersistentId());
			std::vector<wchar_t> environment;
			wchar_t* original = GetEnvironmentStringsW();
			if (!original) return false;
			for (const wchar_t* item = original; *item; item += wcslen(item) + 1)
				if (_wcsnicmp(item, L"CEMU_HOME_", 10) != 0)
					environment.insert(environment.end(), item, item + wcslen(item) + 1);
			FreeEnvironmentStringsW(original);
			auto append = [&](const std::wstring& setting) {
				environment.insert(environment.end(), setting.begin(), setting.end()); environment.push_back(0);
			};
			const auto [gamePath, gameSubpath] = CafeSystem::GetForegroundBaseLocation();
			append(L"CEMU_HOME_SESSION=" + s_mappingName);
			append(L"CEMU_HOME_GAME_PATH=" + gamePath.wstring());
			append(L"CEMU_HOME_GAME_SUBPATH=" + _utf8ToPath(gameSubpath).wstring());
			environment.push_back(0);
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process{};
			Store(s_shared->ready, 0);
			const DWORD priority = Load(s_shared->activation) ? ABOVE_NORMAL_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS;
			if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | priority,
				environment.data(), executable.parent_path().c_str(), &startup, &process)) return false;
			CloseHandle(process.hThread);
			s_peer = process.hProcess;
			return true;
		}

		void SwapPreparedApplet()
		{
			std::swap(s_shared, s_prepared.shared); std::swap(s_mapping, s_prepared.mapping);
			std::swap(s_peer, s_prepared.peer); s_mappingName.swap(s_prepared.mappingName);
		}

		void DisposePreparedApplet()
		{
			if (s_prepared.shared) Store(s_prepared.shared->stop, 1);
			if (s_prepared.shared) UnmapViewOfFile(s_prepared.shared);
			if (s_prepared.mapping) CloseHandle(s_prepared.mapping);
			if (s_prepared.peer) CloseHandle(s_prepared.peer);
			s_prepared = {};
		}

		bool HasDumpedManual()
		{
			std::error_code error;
			if (!fs::is_regular_file(ManualRpx(), error)) return false;
			const auto [path, subpath] = CafeSystem::GetForegroundBaseLocation();
			auto title = subpath.empty() ? std::make_unique<TitleInfo>(path) : std::make_unique<TitleInfo>(path, subpath);
			const auto mount = TitleInfo::GetUniqueTempMountingPath();
			if (!title->IsValid() || !title->Mount(mount, "meta", FSC_PRIORITY_BASE)) return false;
			const bool present = fsc_doesFileExist((mount + "/manual.bfma").c_str());
			title->UnmountAll();
			return present;
		}

		bool PrepareOtherApplet()
		{
			if (s_prepared.shared && Load(s_prepared.shared->stop))
			{
				if (s_prepared.peer && WaitForSingleObject(s_prepared.peer, 0) == WAIT_TIMEOUT) return true;
				DisposePreparedApplet();
			}
			if (s_prepared.attempted) return s_prepared.peer != nullptr;
			s_prepared.attempted = true;
			if (!s_manual && !HasDumpedManual()) return false;
			SwapPreparedApplet(); s_manual = !s_manual;
			bool launched = CreateSession();
			if (launched) { CaptureLayout(); PublishPadRumble(); launched = LaunchApplet(); }
			s_manual = !s_manual; SwapPreparedApplet();
			if (launched) cemuLog_log(LogType::Force, "HOME transition: hidden {} preparation started while the game is already suspended", s_manual ? "HOME" : "manual");
			return launched;
		}

		std::shared_ptr<NativeWiimoteController> PhysicalRemote(const std::shared_ptr<WPADController>& pad)
		{
			if (pad) for (const auto& input : pad->controllers_snapshot())
				if (auto native = std::dynamic_pointer_cast<NativeWiimoteController>(input); native && native->is_connected()) return native;
			return {};
		}

		void UpdateControllers()
		{
			if (!s_shared) return;
			auto& bridge = s_shared->controllers;
			for (unsigned channel=0;channel<7;++channel) if(s_speakerControlActive[channel])
			{
				const auto native=PhysicalRemote(InputManager::instance().get_wpad_controller(channel));
				const auto result=native ? native->speaker_control_result() : std::optional<sint32>{-1};
				if(result)
				{
					Store(bridge.speakerResults[channel],static_cast<std::uint32_t>(*result));
					Store(bridge.speakerCompleted[channel],s_speakerControlTicket[channel]);
					Store(bridge.speakerBusy[channel],0);s_speakerControlActive[channel]=false;
				}
			}
			RemoteCommand command;
			while (PeekRemoteCommand(bridge, command))
			{
				const auto pad = command.channel < InputManager::kMaxWPADControllers ? InputManager::instance().get_wpad_controller(command.channel) : nullptr;
				const auto native = PhysicalRemote(pad);
				if(command.type==RemoteCommandType::SpeakerControl)
				{
					if(command.channel>=7){PopRemoteCommand(bridge);continue;}
					if(s_speakerControlActive[command.channel]) break;
					const auto ticket=Load(bridge.read)+1;
					if(native)
					{
						if(!native->control_speaker(command.value,padscore::WPADGetSpeakerVolume())) break;
						s_speakerControlActive[command.channel]=true;s_speakerControlTicket[command.channel]=ticket;
						Store(bridge.speakerBusy[command.channel],1);
					}
					else {Store(bridge.speakerResults[command.channel],static_cast<uint32>(-1));Store(bridge.speakerCompleted[command.channel],ticket);}
					PopRemoteCommand(bridge);continue;
				}
				if (command.type == RemoteCommandType::Stream && native)
				{
					if (!native->can_send_speaker(false)) { PopRemoteCommand(bridge); continue; }
					if (!native->can_send_speaker()) break;
				}
				switch (command.type)
				{
				case RemoteCommandType::SpeakerControl: break; // Handled by hardware completion above.
				case RemoteCommandType::Stream: if (native && command.length <= 20) native->send_speaker({command.audio.data(), command.length}); break;
				case RemoteCommandType::Volume: padscore::WPADSetSpeakerVolume(static_cast<uint8>(std::min(command.value,127u))); break;
				case RemoteCommandType::Motors: padscore::WPADEnableMotor(command.value != 0); break;
				case RemoteCommandType::Rumble: if (pad) { if (command.value && padscore::WPADMotorsEnabled()) pad->start_rumble(); else pad->stop_rumble(); } break;
				case RemoteCommandType::SyncStart:
					s_syncBusy = true; s_syncCount = 0; ++s_syncGeneration;
					s_syncMask = 0;
					for (unsigned i=0;i<7;++i) if (bridge.remotes[i].connected) s_syncMask |= 1u << i;
					s_syncDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
					InputManager::instance().request_wiimote_pairing();
					break;
				case RemoteCommandType::SyncCancel: s_syncBusy = false; s_syncCount = 0; break;
				case RemoteCommandType::InputSettings:
					if (wxTheApp && !s_inputSettingsPosted.exchange(true)) wxTheApp->CallAfter([] {
						if (auto* main = dynamic_cast<MainWindow*>(wxTheApp->GetTopWindow())) main->OpenInputSettings();
						s_inputSettingsPosted = false;
					});
					break;
				}
				PopRemoteCommand(bridge);
			}
			const auto now = std::chrono::steady_clock::now();
			if (now < s_nextControllerPublish) return;
			s_nextControllerPublish = now + std::chrono::milliseconds(20);
			const auto gamepad = InputManager::instance().get_vpad_controller(0);
			Store(s_shared->padBatteryLevel, gamepad ? gamepad->GetBatteryLevel() : 0xc0);
			Store(bridge.sequence, Load(bridge.sequence) + 1);
			std::uint32_t mask = 0;
			for (unsigned i = 0; i < bridge.remotes.size(); ++i)
			{
				HomeRemoteStatus status;
				if (const auto pad = InputManager::instance().get_wpad_controller(i); pad && pad->is_guest_connected())
				{
					status.connected = 1; status.deviceType = pad->get_device_type(); status.controllerType = pad->type(); mask |= 1u << i;
					if (const auto native = PhysicalRemote(pad))
					{
						const auto battery = native->battery_percent(); status.batteryKnown = battery.has_value(); status.batteryPercent = battery.value_or(0);
						status.speakerReady = native->can_send_speaker(false);
					}
					else { status.batteryKnown = 1; status.batteryPercent = pad->is_battery_low() ? 10 : 100; }
					if (s_gameAudioSuspended.load())
					{
						auto& mirror = s_remoteInputMirrors[i];
						if (!mirror || mirror->type() != pad->type()) mirror = std::dynamic_pointer_cast<WPADController>(ControllerFactory::CreateEmulatedController(i,pad->type()));
						pad->copy_input_bindings_to(*mirror);
						if (auto remote = std::dynamic_pointer_cast<WiimoteController>(mirror)) remote->set_device_type(pad->get_device_type());
						const auto requestedFormat = Load(bridge.dataFormats[i]);
						mirror->set_data_format(requestedFormat == ~0u ? mirror->get_default_data_format() : static_cast<WPADDataFormat>(requestedFormat));
						KPADStatus_t kpad{};
						mirror->KPADRead(kpad, {});
						static_assert(sizeof(kpad) == sizeof(status.input.kpad));
						std::memcpy(status.input.kpad.data(), &kpad, sizeof(kpad));
						static_assert(sizeof(WPADUCStatus_t) <= sizeof(status.input.wpad) && sizeof(WPADMPStatus_t) <= sizeof(status.input.wpad));
						mirror->WPADRead(reinterpret_cast<WPADStatus_t*>(status.input.wpad.data()));
						status.input.format = mirror->get_data_format();status.input.valid = 1;status.input.activation = Load(s_shared->activation);
					}
				}
				TransferRemoteStatus(bridge.remotes[i],status,true);
			}
			if (s_syncBusy && ((mask & ~s_syncMask) || std::chrono::steady_clock::now() >= s_syncDeadline))
			{
				s_syncCount = std::popcount(mask & ~s_syncMask); s_syncBusy = false;
			}
			Store(bridge.volume,padscore::WPADGetSpeakerVolume());Store(bridge.motors,padscore::WPADMotorsEnabled());
			Store(bridge.syncGeneration,s_syncGeneration);Store(bridge.syncBusy,s_syncBusy);Store(bridge.syncCount,s_syncCount);
			Store(bridge.sequence, Load(bridge.sequence) + 1);
		}
	}

	bool IsChild()
	{
		return (HomeMenuProtocol::IsHomeMenuTitle(CafeSystem::GetForegroundTitleId()) || HomeMenuProtocol::IsManualTitle(CafeSystem::GetForegroundTitleId())) && !Environment(L"CEMU_HOME_SESSION").empty();
	}
	bool IsHelperStartup() { return !Environment(L"CEMU_HOME_SESSION").empty(); }

	bool IsConfigured() { return s_parent || s_child; }
	bool RequestInputSettings() { return s_child && SendRemoteCommand(RemoteCommand{RemoteCommandType::InputSettings}); }
	bool SetRemoteDataFormat(std::uint32_t channel, std::uint32_t format)
	{
		if (!s_child || !s_shared || channel >= 7) return false;
		Store(s_shared->controllers.dataFormats[channel], format);return true;
	}
	bool IsGameAudioSuspended() { return s_gameAudioSuspended.load(std::memory_order_acquire); }
	bool IsGameAudioQuiescent() { return s_gameAudioQuiescent.load(std::memory_order_acquire); }
	bool KeepsGameAudioTimeline()
	{
		return s_parent && NbaHomeAudio::Enabled(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
	}
	void SetCloseToWiiUMenu(bool enabled) { s_closeToWiiUMenu = enabled; }
	bool SendRemoteCommand(const RemoteCommand& command, std::uint32_t* ticket)
	{
		if (!s_child || !s_shared || command.length > 20) return false;
		std::scoped_lock lock(s_remoteCommandMutex);
		if(!QueueRemoteCommand(s_shared->controllers, command)) return false;
		if(ticket) *ticket=Load(s_shared->controllers.write);
		return true;
	}
	bool RemoteSpeakerResult(std::uint32_t channel,std::uint32_t ticket,std::int32_t& result)
	{
		if(!s_child || !s_shared || channel>=7) return false;
		auto& bridge=s_shared->controllers;
		if(static_cast<std::int32_t>(Load(bridge.read)-ticket)<0 || Load(bridge.speakerBusy[channel]) || Load(bridge.speakerCompleted[channel])!=ticket) return false;
		result=static_cast<std::int32_t>(Load(bridge.speakerResults[channel]));return true;
	}
	bool CanSendRemoteStream()
	{
		if (!s_child || !s_shared) return false;
		auto& bridge = s_shared->controllers;
		return Load(bridge.write) - Load(bridge.read) < bridge.commands.size() - 4;
	}
	bool GetRemoteStatus(std::uint32_t channel, HomeRemoteStatus& out)
	{
		if (!s_child || !s_shared || channel >= 7) return false;
		std::scoped_lock lock(s_remoteCommandMutex);
		auto& bridge = s_shared->controllers;
		for (unsigned attempt=0; attempt<3; ++attempt)
		{
			const auto sequence = Load(bridge.sequence);
			if (sequence & 1) continue;
			auto& in=bridge.remotes[channel];
			HomeRemoteStatus status;TransferRemoteStatus(in,status,false);
			if (sequence == Load(bridge.sequence)) { s_remoteCache[channel] = status; break; }
		}
		out = s_remoteCache[channel];
		return true;
	}
	bool GetRemoteSettings(std::uint32_t& volume, bool& motors, std::uint32_t& generation, bool& syncing, std::uint32_t& count)
	{
		if (!s_child || !s_shared) return false;
		std::scoped_lock lock(s_remoteCommandMutex);
		auto& bridge=s_shared->controllers;
		for (unsigned attempt=0;attempt<3;++attempt)
		{
			const auto sequence=Load(bridge.sequence);
			if(sequence&1)continue;
			const std::array<std::uint32_t,5> state{Load(bridge.volume),Load(bridge.motors),Load(bridge.syncGeneration),Load(bridge.syncBusy),Load(bridge.syncCount)};
			if(sequence==Load(bridge.sequence)){s_remoteSettings=state;break;}
		}
		volume=s_remoteSettings[0];motors=s_remoteSettings[1]!=0;generation=s_remoteSettings[2];syncing=s_remoteSettings[3]!=0;count=s_remoteSettings[4];return true;
	}

	std::uint64_t GetHelperTimerFrequency()
	{
		const auto name = Environment(L"CEMU_HOME_SESSION");
		if (name.empty()) return 0;
		const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
		if (!mapping) return 0;
		auto* session = static_cast<SharedSession*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedSession)));
		std::uint64_t frequency{};
		if (session && session->magic == SessionMagic && session->version == SessionVersion)
		{
			const auto owner = OpenProcess(SYNCHRONIZE, FALSE, session->ownerPid);
			frequency = ValidatedHelperTimerFrequency(*session, GetCurrentProcessId(), owner && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT);
			if (owner) CloseHandle(owner);
		}
		if (session) UnmapViewOfFile(session);
		CloseHandle(mapping);
		return frequency;
	}

	void Shutdown()
	{
		s_speakerControlActive.fill(false);
		s_gameAudioSuspended.store(false, std::memory_order_release);
		s_gameAudioQuiescent.store(false, std::memory_order_release);
		NbaHomeAudio::traceUntilNs.store(0);
		s_trackFrames = false;
		if (s_parent) DisposePreparedApplet();
		if (s_shared && s_parent) Store(s_shared->stop, 1);
		if (s_shared) UnmapViewOfFile(s_shared);
		if (s_mapping) CloseHandle(s_mapping);
		if (s_peer) CloseHandle(s_peer);
		s_shared = nullptr; s_mapping = s_peer = nullptr;
		s_child = s_parent = false; s_phase = Phase::Idle;
		s_release.Reset(); s_activation.Reset(); s_resumeActivation.Reset(); s_requestedAction.store(None);
		s_deniedActivation.Reset();
		if (s_retainedMetadata) s_retainedMetadata->UnmountAll();
		s_retainedMetadata.reset();
		s_edges.Reset(); s_caller.store(0); s_seenActivation = 0;
		s_componentInput.Reset(); s_padComponentInput.Reset(); s_manualMetadataBound = false;
		for (auto& mirror : s_remoteInputMirrors) mirror.reset();
		s_ignoreHome = s_closePosted = false;
		s_preloadTried = false; s_seenResume = 0;
		s_manual = false;
		s_manualForeground = false; s_manualShown = false;
		s_componentRetiring = false; s_nativeFrames = 0; s_shownActivation = 0;
		s_frameBaseline = 0; s_showPending = s_handoffPending = false;
		s_rumbleRevision = 0; s_rumbleLevels.clear();
		s_syncBusy = false; s_syncGeneration = s_syncMask = s_syncCount = 0;
		s_nextControllerPublish = {}; s_remoteCache = {};
		s_remoteSettings = {64,1,0,0,0};
		s_seenLayout = 0;
	}

	void Initialize()
	{
		Shutdown();
		if (IsChild())
		{
			s_child = true;
			s_manual = HomeMenuProtocol::IsManualTitle(CafeSystem::GetForegroundTitleId());
			s_caller = s_manual ? 5 : 0;
			s_trackFrames = true;
			s_mappingName = Environment(L"CEMU_HOME_SESSION");
			s_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, s_mappingName.c_str());
			if (s_mapping) s_shared = static_cast<SharedSession*>(MapViewOfFile(s_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedSession)));
			if (s_shared && s_shared->magic == SessionMagic && s_shared->version == SessionVersion && IsGameTitle(s_shared->gameTitle))
				s_peer = OpenProcess(SYNCHRONIZE, FALSE, s_shared->ownerPid);
			if (!s_peer || WaitForSingleObject(s_peer, 0) != WAIT_TIMEOUT || Load(s_shared->stop))
			{
				cemuLog_log(LogType::Force, "HOME prototype: live owner validation failed"); CloseApplet(); return;
			}
			cemuLog_log(LogType::Force, "HOME prototype: native applet bound to live game {:016x}, owner PID {}", s_shared->gameTitle, s_shared->ownerPid);
			BindRetainedMetadata();
		}
		else if (IsGameTitle(CafeSystem::GetForegroundTitleId()))
		{
			std::error_code executableError, homeError;
			s_parent = Environment(L"CEMU_NATIVE_HOME_PROTOTYPE") == L"1" ||
				(fs::is_regular_file(ChildExecutable(), executableError) && fs::is_regular_file(HomeRpx(), homeError));
			if (s_parent) cemuLog_log(LogType::Force, "Native HOME: supervised applet enabled; ordinary title-launch path is not used for suspension");
		}
	}

	bool ReleaseForeground(std::uint32_t core) { return IsConfigured() && s_release.Notify(core); }

	bool RequestReturn(bool closeSoftware)
	{
		if (!s_child || !s_shared) return false;
		const auto action = closeSoftware ? CloseSoftware : (s_manual ? ReturnHome : Resume);
		if (s_requestedAction.load() != None) return s_requestedAction.load() == action;
		s_release.Begin();
		s_requestedAction.store(action);
		if (!Queue(SysMessageId::MsgReleaseForeground))
		{
			s_requestedAction.store(None); s_release.Reset(); return false;
		}
		Store(s_shared->action, action);
		// In the process-isolated prototype, retire only HOME on Resume. The owner
		// waits for that normal exit before acquiring foreground in the retained game.
		if (!closeSoftware && s_manual) s_componentRetiring = true;
		else if (!closeSoftware) CloseApplet();
		cemuLog_log(LogType::Force, "HOME prototype: native {} committed; {}", closeSoftware ? "Close Software" : s_manual ? "Back to HOME" : "Resume",
			closeSoftware ? "owner will close software using the startup preference" : s_manual ? "retaining manual windows until HOME is visible" : "closing HOME helper normally before game reacquire");
		return true;
	}

	bool RequestRootSwitch()
	{
		if (!s_child || !s_shared) return false;
		// Before first activation the resident applet may park at the kernel.
		// After activation, its native Resume handler uses this same UPID 1 route.
		return s_componentRetiring.load() || s_seenActivation.load() == 0 || RequestReturn(false);
	}

	std::uint64_t MainApplicationTitle()
	{
		return s_child && s_shared && s_peer && WaitForSingleObject(s_peer, 0) == WAIT_TIMEOUT ? s_shared->gameTitle : 0;
	}

	bool GetCaller(std::uint32_t& caller) { caller = s_caller.load(); return caller != 0; }

	bool ReadMenuButtons(std::uint32_t& hold, std::uint32_t& pressed, std::uint32_t& released)
	{
		if (!s_child || !s_shared) return false;
		hold = !s_componentRetiring.load() && Load(s_shared->shown) == Load(s_shared->activation) && Load(s_shared->activation) ? Load(s_shared->buttons) : 0;
		s_edges.Read(hold, pressed, released); return true;
	}

	std::uint8_t GetGamePadBatteryLevel()
	{
		return s_child && s_shared ? static_cast<std::uint8_t>(Load(s_shared->padBatteryLevel)) : 0xc0;
	}

	bool GetGamePadRumble(bool& enabled)
	{
		if (!s_child || !s_shared) return false;
		enabled = Load(s_shared->padRumble) != 0;
		return true;
	}

	bool SetGamePadRumble(bool enabled)
	{
		if (!s_child || !s_shared) return false;
		Store(s_shared->padRumble, enabled);
		std::atomic_ref(s_shared->padRumbleRevision).fetch_add(1, std::memory_order_release);
		return true;
	}

	bool RequestManual()
	{
		if (!s_child || !s_shared || s_manual || s_requestedAction.load() != None) return false;
		CafeSystem::MlcStorageMountAllTitles();
		const auto title = s_shared->gameTitle;
		const auto path = fmt::format("/vol/storage_mlc01/usr/title/{:08x}/{:08x}/meta/manual.bfma", static_cast<uint32>(title >> 32), static_cast<uint32>(title));
		std::error_code error;
		if (!fs::is_regular_file(ManualRpx(), error) || !fsc_doesFileExist(path.c_str()))
		{
			cemuLog_log(LogType::Force, "HOME manual: native viewer or dumped manual missing; HOME remains open: {}", path);
			return false;
		}
		s_release.Begin();
		if (!Queue(SysMessageId::MsgReleaseForeground)) { s_release.Reset(); return false; }
		s_componentRetiring = true;
		s_requestedAction.store(OpenManual); Store(s_shared->action, OpenManual);
		cemuLog_log(LogType::Force, "HOME manual: original viewer requested for retained title {:016x}; retaining HOME windows until the reader is visible", title);
		return true;
	}

	bool BindManualMetadata(std::string_view source, std::string_view destination)
	{
		if (!s_child || !s_manual || !s_shared || !s_retainedMetadata || s_manualMetadataBound) return false;
		while (source.size() > 1 && source.back() == '/') source.remove_suffix(1);
		while (destination.size() > 1 && destination.back() == '/') destination.remove_suffix(1);
		const auto expected = CafeSystem::GetMlcStoragePath(s_shared->gameTitle) + "/meta";
		if (source != expected || destination != "/vol/meta_priv") return false;
		if (!s_retainedMetadata->Mount(destination, "meta", FSC_PRIORITY_BASE)) return false;
		s_manualMetadataBound = true;
		cemuLog_log(LogType::Force, "HOME manual: original retained-game metadata bind-mounted at {}", destination);
		return true;
	}

	bool UnbindManualMetadata(std::string_view path)
	{
		while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
		if (!s_child || !s_manual || !s_retainedMetadata || !s_manualMetadataBound || path != "/vol/meta_priv") return false;
		s_retainedMetadata->Unmount("/vol/meta_priv"); s_manualMetadataBound = false;
		return true;
	}

	void SystemMessageReceived(std::uint32_t id)
	{
		if (!s_child || !s_manual || !s_shared) return;
		if (id == stdx::to_underlying(SysMessageId::MsgAcquireForeground))
		{
			s_manualForeground = true;
			Store(s_shared->ready, 1);
			cemuLog_log(LogType::Force, "HOME manual: original viewer consumed foreground-acquire message");
		}
		else if (id == stdx::to_underlying(SysMessageId::MsgReleaseForeground)) s_manualForeground = false;
	}

	void NotifyScanout()
	{
		if (s_trackFrames.load(std::memory_order_relaxed)) s_nativeFrames.fetch_add(1, std::memory_order_release);
	}

	void Poll()
	{
		if (s_child)
		{
			if (!s_shared || !s_peer || Load(s_shared->stop) || WaitForSingleObject(s_peer, 0) != WAIT_TIMEOUT) { CloseApplet(); return; }
			const auto activation = Load(s_shared->activation);
			Store(s_shared->shown, s_shownActivation.load());
			if (s_manual && s_nativeFrames.load() && !Load(s_shared->ready)) Store(s_shared->ready, 1);
			if (s_manual && activation != s_seenActivation && !s_closePosted)
			{
				s_caller = 5;
				// Unlike the parked HOME helper, a fresh reader already initializes
				// in foreground. A synthetic acquire repeats its full MEM1 allocation.
				s_manualForeground = true; s_seenActivation = activation;
				s_showPending = true;
				cemuLog_log(LogType::Force, "HOME manual: fresh foreground reader activated without a duplicate acquire message");
			}
			if (s_showPending && !s_componentRetiring.load() && !s_closePosted &&
				(s_manual ? s_nativeFrames.load() != 0 : s_nativeFrames.load() > s_frameBaseline))
			{
				s_manualShown = s_manual; s_showPending = false; ShowApplet(true);
			}
			if (proc_ui::ProcUIIsRunning() && !proc_ui::ProcUIInForeground() && !Load(s_shared->ready))
			{
				Store(s_shared->ready, 1);
				cemuLog_log(LogType::Force, "HOME timing: native helper prepared in background; no game suspension requested");
			}
			SurfaceLayout layout; std::uint32_t sequence{};
			if (!s_closePosted && !s_componentRetiring.load() && s_seenActivation && s_shownActivation.load() == s_seenActivation && ReadLayout(*s_shared, layout, sequence) && sequence != s_seenLayout)
			{
				s_seenLayout = sequence; ShowApplet(true, false);
			}
			if (!s_manual && activation != s_seenActivation && proc_ui::ProcUIIsRunning() && !proc_ui::ProcUIInForeground())
			{
				s_caller = s_manual ? 5 : 15;
				if (Queue(SysMessageId::MsgAcquireForeground))
				{
					s_frameBaseline = s_nativeFrames.load(); s_showPending = true;
					s_seenActivation = activation; s_requestedAction.store(None); s_release.Reset();
					cemuLog_log(LogType::Force, "HOME prototype: native foreground requested for live game {:016x}", s_shared->gameTitle);
				}
			}
			if (s_requestedAction.load() != None && s_release.Complete())
			{
				Store(s_shared->action, s_requestedAction.exchange(None)); s_release.Reset();
				if (!s_componentRetiring.load()) ShowApplet(false);
			}
			const auto resume = Load(s_shared->resumeRequest);
			if (resume != s_seenResume && s_seenActivation && !s_closePosted && !s_componentRetiring.load() && (s_manual ? s_manualForeground.load() : proc_ui::ProcUIInForeground()))
			{
				if (RequestReturn(false)) s_seenResume = resume;
			}
			return;
		}
		if (!s_parent) return;
		const auto now = std::chrono::steady_clock::now();
		if (s_shared) ApplyPadRumble();
		if (s_shared) UpdateControllers();
		// Prepare the original applet while the game runs, never acquire foreground
		// or suspend the game until the user actually presses HOME.
		if (s_phase == Phase::Idle && !s_preloadTried)
		{
			s_preloadTried = true;
			if (CreateSession())
			{
				Store(s_shared->activation, 0); Store(s_shared->resumeRequest, 0); Store(s_shared->action, None);
				Store(s_shared->stop, 0); Store(s_shared->shown, 0);
				PublishPadRumble();
				if (LaunchApplet()) cemuLog_log(LogType::Force, "HOME timing: hidden preparation started for selected account {:08x}", ActiveSettings::GetPersistentId());
			}
		}
		if (s_shared && now >= s_nextLayoutCheck)
		{
			CaptureLayout(); s_nextLayoutCheck = now + std::chrono::milliseconds(100);
		}
		if (s_handoffPending)
		{
			if (Load(s_shared->shown) == Load(s_shared->activation) && Load(s_shared->activation))
			{
				Store(s_prepared.shared->stop, 1); s_handoffPending = false;
				cemuLog_log(LogType::Force, "HOME transition: incoming native windows shown; outgoing helper may now close");
			}
			else if (now - s_phaseStart > std::chrono::seconds(15))
			{
				s_phase = Phase::Resume;
				cemuLog_log(LogType::Force, "HOME transition: native window handoff timed out; recovering retained game");
			}
		}
		if (s_phase == Phase::Home && !s_handoffPending && Load(s_shared->shown)) PrepareOtherApplet();
		const auto rawButtons = ReadButtons();
		const auto rawPadButtons = ReadButtons(true);
		const auto padButtons = s_padComponentInput.Filter(rawPadButtons);
		const auto buttons = s_componentInput.Filter(rawButtons);
		if (s_shared)
		{
			if (!(buttons & 2)) s_ignoreHome = false;
			const bool visible = Load(s_shared->activation) && Load(s_shared->shown) == Load(s_shared->activation);
			Store(s_shared->buttons, visible ? (s_ignoreHome ? padButtons & ~2u : padButtons) : 0);
		}
		if (s_resumeActivation.Poll(buttons & 2, s_phase == Phase::Home && !s_ignoreHome && Load(s_shared->shown) == Load(s_shared->activation)))
		{
			Store(s_shared->resumeRequest, Load(s_shared->resumeRequest) + 1);
			s_resumeActivation.Acknowledge();
		}
		if (s_deniedActivation.Poll(buttons & 2, s_phase == Phase::Idle && !OSIsHomeButtonMenuEnabled()))
		{
			s_deniedActivation.Acknowledge();
			cemuLog_log(LogType::Force, "HOME input: title currently disables HOME; honoring title policy (ProcUI HLE initialized={}, foreground={})", proc_ui::ProcUIIsRunning(), proc_ui::ProcUIInForeground());
		}
		const bool diagnosticRelease = NbaHomeCheck::scenario.RequestRelease(NbaHomeCheck::CurrentMode(),
			KeepsGameAudioTimeline() && OSIsHomeButtonMenuEnabled(), s_phase == Phase::Idle, NbaHomeCheck::movieFrames.load());
		if (s_activation.Poll(buttons & 2, s_phase == Phase::Idle && OSIsHomeButtonMenuEnabled()) || diagnosticRelease)
		{
			if (!CreateSession())
			{
				s_activation.Acknowledge();
				return;
			}
			s_release.Begin();
			PublishPadRumble();
			if (s_peer) SetPriorityClass(s_peer, ABOVE_NORMAL_PRIORITY_CLASS);
			if (Queue(SysMessageId::MsgReleaseForeground))
			{
				s_gameAudioSuspended.store(true, std::memory_order_release);
				std::atomic_ref(s_shared->activationTick).store(GetTickCount64());
				s_phase = Phase::ReleasingGame; s_phaseStart = std::chrono::steady_clock::now(); s_activation.Acknowledge();
				cemuLog_log(LogType::Force, "HOME prototype: game release queued; memory and title remain loaded");
			}
		}
		if (s_phase == Phase::ReleasingGame && s_release.Complete())
		{
			// Release callbacks may need AX; freeze it only after every core released.
			s_gameAudioQuiescent.store(true, std::memory_order_release);
			if (KeepsGameAudioTimeline()) NbaMovieResumeTime::recovery.Suspend(OSGetTime());
			CaptureLayout();
			SurfaceLayout layout; std::uint32_t sequence{};
			if (!NbaHomeCheck::scenario.Requested() && (!ReadLayout(*s_shared, layout, sequence) || !layout.tv.Valid()))
			{
				if (now - s_phaseStart > std::chrono::seconds(10)) s_phase = Phase::Resume;
				return;
			}
			s_release.Reset(); s_ignoreHome = true;
			s_componentInput.Arm(rawButtons);
			s_padComponentInput.Arm(rawPadButtons);
			Store(s_shared->buttons, 0); Store(s_shared->action, None);
			if (NbaHomeCheck::scenario.Requested())
			{
				NbaHomeCheck::scenario.Released(NbaHomeCheck::NowNs());
				s_phase = Phase::Home;
				cemuLog_log(LogType::Force, "[COMPAT:NBA-HOME:CHECK] All game cores released; helper stays parked. Parent-callback isolation check, NOT full native HOME UI coverage.");
			}
			else
			{
				Store(s_shared->activation, Load(s_shared->activation) + 1);
				s_phase = LaunchApplet() ? Phase::Home : Phase::Resume;
				cemuLog_log(LogType::Force, "HOME prototype: all three game cores released; native launch {}", s_phase == Phase::Home ? "started" : "failed, resuming game");
			}
		}
		if (s_phase == Phase::ReleasingGame && std::chrono::steady_clock::now() - s_phaseStart > std::chrono::seconds(10))
		{
			s_phase = Phase::Resume;
			cemuLog_log(LogType::Force, "HOME prototype: game did not finish foreground release; canceling with an acquire message");
		}
		if (s_phase == Phase::Home)
		{
			const auto action = Load(s_shared->action);
			if (action == CloseSoftware) s_phase = Phase::Close;
			else if (!s_handoffPending && (action == OpenManual || action == ReturnHome))
			{
				s_phase = action == OpenManual ? Phase::OpenManual : Phase::ReturnHome; s_phaseStart = now;
			}
			else if (!s_peer || WaitForSingleObject(s_peer, 0) != WAIT_TIMEOUT)
			{
				DWORD exitCode{};
				const bool failedManual = s_manual && s_peer && (!GetExitCodeProcess(s_peer, &exitCode) || exitCode != 0);
				s_phase = s_manual && !failedManual && !s_handoffPending ? Phase::ReturnHome : Phase::Resume;
				if (failedManual) cemuLog_log(LogType::Force, "HOME manual: reader exited abnormally (0x{:08x}); returning foreground to the retained game", exitCode);
			}
		}
		if (s_phase == Phase::OpenManual || s_phase == Phase::ReturnHome)
		{
			if (!PrepareOtherApplet()) s_phase = Phase::Resume;
			else if (s_prepared.shared && !Load(s_prepared.shared->stop) && s_prepared.peer && WaitForSingleObject(s_prepared.peer, 0) == WAIT_TIMEOUT &&
				CanActivatePreparedComponent(s_phase == Phase::OpenManual, std::chrono::duration_cast<std::chrono::milliseconds>(now - s_phaseStart).count()))
			{
				SwapPreparedApplet(); s_manual = s_phase == Phase::OpenManual;
				PublishPadRumble();
				Store(s_shared->action, None); Store(s_shared->activation, 1); Store(s_shared->resumeRequest, 0);
				std::atomic_ref(s_shared->activationTick).store(GetTickCount64()); CaptureLayout();
				s_ignoreHome = true; s_componentInput.Arm(rawButtons); s_padComponentInput.Arm(rawPadButtons); Store(s_shared->buttons, 0);
				SetPriorityClass(s_peer, ABOVE_NORMAL_PRIORITY_CLASS);
				s_handoffPending = true; s_phase = Phase::Home; s_phaseStart = now;
				cemuLog_log(LogType::Force, "HOME transition: activating prepared {}; outgoing native screen remains in place", s_manual ? "manual" : "HOME");
			}
			else if (now - s_phaseStart > std::chrono::seconds(15)) s_phase = Phase::Resume;
		}
		if (s_phase == Phase::Home && NbaHomeCheck::scenario.RequestResume(NbaHomeCheck::NowNs()))
		{
			s_phase = Phase::Resume;
			cemuLog_log(LogType::Force, "[COMPAT:NBA-HOME:CHECK] Three-second parent callback hold complete; requesting normal game reacquire.");
		}
		if (s_phase == Phase::Resume)
		{
			DisposePreparedApplet(); s_handoffPending = false;
			Store(s_shared->stop, 1);
			if (s_peer && WaitForSingleObject(s_peer, 0) == WAIT_TIMEOUT) return;
			s_caller = 5;
			const bool restoreForeground = NbaHomeFocus::Requested(KeepsGameAudioTimeline(), Load(s_shared->action) == Resume);
			if (Queue(SysMessageId::MsgAcquireForeground))
			{
				if (KeepsGameAudioTimeline()) NbaHomeAudio::BeginResumeTrace();
				s_gameAudioQuiescent.store(false, std::memory_order_release);
				s_gameAudioSuspended.store(false, std::memory_order_release);
				s_phase = Phase::Idle; s_release.Reset(); Store(s_shared->action, None); s_preloadTried = false; s_manual = false;
				cemuLog_log(LogType::Force, "HOME prototype: foreground returned to the same loaded game {:016x}", CafeSystem::GetForegroundTitleId());
				RestoreGameWindow(restoreForeground);
			}
		}
		else if (s_phase == Phase::Close)
		{
			if (s_closeToWiiUMenu.load())
			{
				s_phase = Phase::Idle;
				cemuLog_log(LogType::Force, "HOME prototype: Close Software returning to Wii U Menu (startup preference enabled)");
				__LaunchByTitleId(_SYSGetSystemApplicationTitleId(0), 0, nullptr);
			}
			else if (!s_closePosted && wxTheApp)
			{
				s_closePosted = true;
				cemuLog_log(LogType::Force, "HOME prototype: Close Software returning to Cemu game list");
				wxTheApp->CallAfter([] {
					if (auto* main = dynamic_cast<MainWindow*>(wxTheApp->GetTopWindow())) main->StopGame(false);
				});
			}
		}
	}
}
#else
namespace coreinit::NativeHomeSession
{
	bool IsConfigured() { return false; }
	bool IsChild() { return false; }
	bool RequestInputSettings() { return false; }
	bool SetRemoteDataFormat(std::uint32_t, std::uint32_t) { return false; }
	bool IsGameAudioSuspended() { return false; }
	bool IsGameAudioQuiescent() { return false; }
	bool KeepsGameAudioTimeline() { return false; }
	bool IsHelperStartup() { return false; }
	std::uint64_t GetHelperTimerFrequency() { return 0; }
	void SetCloseToWiiUMenu(bool) {}
	void Initialize() {}
	void Shutdown() {}
	void Poll() {}
	void NotifyScanout() {}
	bool GetRemoteStatus(std::uint32_t, HomeRemoteStatus&) { return false; }
	bool SendRemoteCommand(const RemoteCommand&, std::uint32_t*) { return false; }
	bool RemoteSpeakerResult(std::uint32_t,std::uint32_t,std::int32_t&) { return false; }
	bool CanSendRemoteStream() { return false; }
	bool GetRemoteSettings(std::uint32_t&, bool&, std::uint32_t&, bool&, std::uint32_t&) { return false; }
	bool ReleaseForeground(std::uint32_t) { return false; }
	bool RequestReturn(bool) { return false; }
	bool RequestRootSwitch() { return false; }
	std::uint64_t MainApplicationTitle() { return 0; }
	bool GetCaller(std::uint32_t& caller) { caller = 0; return false; }
	bool ReadMenuButtons(std::uint32_t&, std::uint32_t&, std::uint32_t&) { return false; }
	std::uint8_t GetGamePadBatteryLevel() { return 0xc0; }
	bool GetGamePadRumble(bool&) { return false; }
	bool SetGamePadRumble(bool) { return false; }
	bool RequestManual() { return false; }
	bool BindManualMetadata(std::string_view, std::string_view) { return false; }
	bool UnbindManualMetadata(std::string_view) { return false; }
	void SystemMessageReceived(std::uint32_t) {}
}
#endif
