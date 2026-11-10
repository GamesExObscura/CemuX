#pragma once
#include "WiimoteAudioWriteQueue.h"
#include <cassert>
#include <vector>

namespace WiimoteAudioWriteQueueTests
{
    struct FakeWriter
    {
        struct Token
        {
            std::array<std::uint8_t,22> snapshot{};
            const std::uint8_t* buffer{};
            unsigned sequence{};
            bool pending{};
        };
        unsigned attempts{},pending{},maximumPending{};
        int failBegin=-1,failFinish=-1;
        std::vector<unsigned> completions;
        FakeWriter(){completions.reserve(64);}
        bool Begin(Token& token,std::span<const std::uint8_t> bytes) noexcept
        {
            assert(!token.pending);
            token.sequence=attempts++;
            if(static_cast<int>(token.sequence)==failBegin)return false;
            token.buffer=bytes.data();std::copy(bytes.begin(),bytes.end(),token.snapshot.begin());
            token.pending=true;++pending;maximumPending=std::max(maximumPending,pending);
            assert(pending<=2);return true;
        }
        bool Finish(Token& token,std::size_t wanted) noexcept
        {
            assert(token.pending && wanted==22);
            assert(std::equal(token.snapshot.begin(),token.snapshot.end(),token.buffer));
            completions.push_back(token.sequence);token.pending=false;--pending;
            return static_cast<int>(token.sequence)!=failFinish;
        }
    };
    inline auto Report(unsigned value)
    {
        std::array<std::uint8_t,22> report{};report.fill(static_cast<std::uint8_t>(value));
        report[0]=0x18;report[1]=160;return report;
    }
    inline void Run()
    {
        FakeWriter writer;
        {
            WiimoteAudioWriteQueue queue(writer);
            auto report=Report(1);assert(queue.Submit(report));report.fill(0xff);
            assert(queue.Submit(Report(2)) && writer.completions.empty());
            assert(queue.Submit(Report(3)) && writer.completions==std::vector<unsigned>{0});
            assert(queue.Drain());
            assert(writer.completions==std::vector<unsigned>({0,1,2}) && writer.pending==0);
            for(unsigned i=0;i<29;++i)assert(queue.Submit(Report(i)));
        }
        assert(writer.pending==0 && writer.maximumPending==2 && writer.completions.size()==32);
        for(unsigned i=0;i<32;++i)assert(writer.completions[i]==i);
        FakeWriter partial;partial.failFinish=0;
        {
            WiimoteAudioWriteQueue queue(partial);
            assert(queue.Submit(Report(1)) && queue.Submit(Report(2)));
            assert(!queue.Submit(Report(3)) && partial.attempts==2);
            assert(!queue.Drain() && partial.pending==0 && partial.completions.size()==2);
            assert(!queue.Submit(Report(4)));
        }
        FakeWriter rejected;rejected.failBegin=1;
        {
            WiimoteAudioWriteQueue queue(rejected);
            assert(queue.Submit(Report(1)) && !queue.Submit(Report(2)));
        }
        assert(rejected.pending==0 && rejected.completions==std::vector<unsigned>{0});
        FakeWriter invalid;
        {
            WiimoteAudioWriteQueue queue(invalid);
            for(unsigned command:{0x12,0x14,0x16,0x17,0x19}){
                auto report=Report(0);report[0]=static_cast<std::uint8_t>(command);
                assert(!queue.Submit(report));
            }
            auto report=Report(0);report[1]=0;assert(!queue.Submit(report));
            report[1]=168;assert(!queue.Submit(report));
            assert(!queue.Submit(std::span(report).first(21)));
            assert(invalid.attempts==0 && queue.Submit(Report(4)));
        }
        assert(invalid.pending==0 && invalid.completions.size()==1);
        FakeWriter exception;
        try{
            WiimoteAudioWriteQueue queue(exception);
            assert(queue.Submit(Report(1)) && queue.Submit(Report(2)));
            throw 1;
        }catch(int){}
        assert(exception.pending==0 && exception.completions==std::vector<unsigned>({0,1}));
    }
}
