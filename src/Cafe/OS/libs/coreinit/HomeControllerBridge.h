#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace coreinit::NativeHomeSession
{
    struct HomeRemoteInput
    {
        std::uint32_t valid{}, format{}, activation{};
        std::array<std::uint32_t,60> kpad{};
        std::array<std::uint32_t,24> wpad{};
    };
    struct HomeRemoteStatus
    {
        std::uint32_t connected{}, deviceType{253}, controllerType{3}, batteryPercent{}, batteryKnown{}, speakerReady{};
        HomeRemoteInput input;
    };
    enum class RemoteCommandType : std::uint32_t {SpeakerControl, Stream, Volume, Motors, Rumble, SyncStart, SyncCancel, InputSettings};
    struct RemoteCommand
    {
        RemoteCommandType type{};
        std::uint32_t channel{}, value{}, length{};
        std::array<std::uint8_t,20> audio{};
    };
    struct HomeControllerBridge
    {
        std::uint32_t sequence{};
        std::array<HomeRemoteStatus,7> remotes{};
        std::array<std::uint32_t,7> dataFormats{~0u,~0u,~0u,~0u,~0u,~0u,~0u};
        std::uint32_t volume{64}, motors{1}, syncGeneration{}, syncBusy{}, syncCount{};
        std::uint32_t read{}, write{};
        std::array<std::uint32_t,7> speakerResults{},speakerCompleted{},speakerBusy{};
        std::array<RemoteCommand,16> commands{};
    };
    inline void TransferRemoteStatus(HomeRemoteStatus& shared, HomeRemoteStatus& value, bool publishing)
    {
        const auto transfer = [publishing](std::uint32_t& shared, std::uint32_t& value) {
            if (publishing) std::atomic_ref(shared).store(value, std::memory_order_relaxed);
            else value = std::atomic_ref(shared).load(std::memory_order_relaxed);
        };
        transfer(shared.connected,value.connected);transfer(shared.deviceType,value.deviceType);
        transfer(shared.controllerType,value.controllerType);transfer(shared.batteryPercent,value.batteryPercent);
        transfer(shared.batteryKnown,value.batteryKnown);transfer(shared.speakerReady,value.speakerReady);
        transfer(shared.input.valid,value.input.valid);transfer(shared.input.format,value.input.format);
        transfer(shared.input.activation,value.input.activation);
        for(size_t i=0;i<value.input.kpad.size();++i) transfer(shared.input.kpad[i],value.input.kpad[i]);
        for(size_t i=0;i<value.input.wpad.size();++i) transfer(shared.input.wpad[i],value.input.wpad[i]);
    }
    struct RemoteInputGate
    {
        std::uint32_t activation{~0u}, blocked{};
        std::uint32_t Filter(std::uint32_t generation, std::uint32_t held)
        {
            if (generation != activation) { activation=generation;blocked=held; }
            blocked &= held;
            return held & ~blocked;
        }
    };
    inline bool QueueRemoteCommand(HomeControllerBridge& bridge, const RemoteCommand& command)
    {
        const auto write=std::atomic_ref(bridge.write).load(std::memory_order_relaxed);
        const auto limit = bridge.commands.size() - (command.type == RemoteCommandType::Stream ? 4 : 0);
        if(write-std::atomic_ref(bridge.read).load(std::memory_order_acquire)>=limit) return false;
        bridge.commands[write%bridge.commands.size()]=command;
        std::atomic_ref(bridge.write).store(write+1,std::memory_order_release);
        return true;
    }
    inline bool PeekRemoteCommand(HomeControllerBridge& bridge, RemoteCommand& command)
    {
        const auto read=std::atomic_ref(bridge.read).load(std::memory_order_relaxed);
        if(read==std::atomic_ref(bridge.write).load(std::memory_order_acquire)) return false;
        command=bridge.commands[read%bridge.commands.size()];return true;
    }
    inline void PopRemoteCommand(HomeControllerBridge& bridge)
    {
        std::atomic_ref(bridge.read).fetch_add(1,std::memory_order_release);
    }
}
