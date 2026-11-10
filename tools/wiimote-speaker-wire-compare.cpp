#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <SDL3/SDL_hidapi.h>
#include "input/api/Wiimote/WiimoteOutputSchedule.h"
#include "input/api/Wiimote/WiimoteOutputWait.h"
#include "input/api/Wiimote/WiimoteSpeakerProtocol.h"
#include "Cafe/OS/libs/padscore/YamahaAdpcm.h"
#include "WiimoteAudioWriteQueue.h"
#include "WiimoteAudioWriteQueueTests.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using Bytes=std::vector<std::uint8_t>;
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;

void LogBytes(std::ostream& log,std::span<const std::uint8_t> bytes){
    constexpr char hex[]="0123456789abcdef";
    for(auto byte:bytes)log<<hex[byte>>4]<<hex[byte&15];
}

struct Transport {
    virtual ~Transport()=default;
    virtual int Write(std::span<const std::uint8_t>)=0;
    virtual int Read(std::span<std::uint8_t>,unsigned)=0;
    virtual bool FlushAudio(){return true;}
    virtual void DescribeAudio(std::ostream&){}
};
bool CompleteWrite(int result,std::size_t wanted){return result>=0 && static_cast<std::size_t>(result)>=wanted;}
struct SdlTransport final:Transport {
    SDL_hid_device* device;
    explicit SdlTransport(const std::string& path):device(SDL_hid_open_path(path.c_str())){
        if(!device)throw std::runtime_error("SDL could not open the selected controller");
    }
    ~SdlTransport(){SDL_hid_close(device);}
    int Write(std::span<const std::uint8_t> data)override{return SDL_hid_write(device,data.data(),data.size());}
    int Read(std::span<std::uint8_t> data,unsigned timeout)override{return SDL_hid_read_timeout(device,data.data(),data.size(),static_cast<int>(timeout));}
};
std::wstring Wide(const std::string& path){
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.c_str(),-1,nullptr,0);
    if(count<=0)throw std::runtime_error("Invalid HID interface path");
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.c_str(),-1,result.data(),count);
    return result;
}
struct WinTransport final:Transport {
    HANDLE device=INVALID_HANDLE_VALUE;
    OVERLAPPED read{},write{};
    std::array<std::uint8_t,22> input{};
    bool pendingRead=false;
    explicit WinTransport(const std::string& path){
        read.hEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        write.hEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!read.hEvent || !write.hEvent){Close();throw std::runtime_error("Cannot create I/O events");}
        device=CreateFileW(Wide(path).c_str(),GENERIC_READ|GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(device==INVALID_HANDLE_VALUE){Close();throw std::runtime_error("Native Windows open failed");}
    }
    void Close(){
        if(device!=INVALID_HANDLE_VALUE){
            if(pendingRead){DWORD ignored{};CancelIoEx(device,&read);GetOverlappedResult(device,&read,&ignored,TRUE);pendingRead=false;}
            CloseHandle(device);device=INVALID_HANDLE_VALUE;
        }
        if(read.hEvent){CloseHandle(read.hEvent);read.hEvent=nullptr;}
        if(write.hEvent){CloseHandle(write.hEvent);write.hEvent=nullptr;}
    }
    ~WinTransport(){Close();}
    int Write(std::span<const std::uint8_t> data)override{
        DWORD count{};
        ResetEvent(write.hEvent);
        if(WriteFile(device,data.data(),static_cast<DWORD>(data.size()),&count,&write))return count;
        if(GetLastError()!=ERROR_IO_PENDING)return -1;
        if(WaitForSingleObject(write.hEvent,1000)!=WAIT_OBJECT_0){
            CancelIoEx(device,&write);GetOverlappedResult(device,&write,&count,TRUE);return -1;
        }
        return GetOverlappedResult(device,&write,&count,FALSE)?static_cast<int>(count):-1;
    }
    int Read(std::span<std::uint8_t> data,unsigned timeout)override{
        DWORD count{};
        if(!pendingRead){
            ResetEvent(read.hEvent);
            if(ReadFile(device,input.data(),static_cast<DWORD>(input.size()),&count,&read)){
                std::copy_n(input.begin(),std::min<std::size_t>(count,data.size()),data.begin());return count;
            }
            if(GetLastError()!=ERROR_IO_PENDING)return -1;
            pendingRead=true;
        }
        const DWORD status=WaitForSingleObject(read.hEvent,timeout);
        if(status==WAIT_TIMEOUT)return 0;
        if(status!=WAIT_OBJECT_0)return -1;
        const bool ok=GetOverlappedResult(device,&read,&count,FALSE)!=FALSE;
        pendingRead=false;
        if(!ok)return -1;
        std::copy_n(input.begin(),std::min<std::size_t>(count,data.size()),data.begin());return count;
    }
};

struct OverlappedAudioWriter {
    struct Token {
        OVERLAPPED request{};
        bool immediate{};
        DWORD bytes{},error{};
        unsigned sequence{};
        Clock::time_point start{};
        double beginCallMs{};
        Token(){
            request.hEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!request.hEvent)throw std::runtime_error("Cannot create audio I/O event");
        }
        ~Token(){CloseHandle(request.hEvent);}
        Token(const Token&)=delete;
        Token& operator=(const Token&)=delete;
    };
    struct Completion {unsigned sequence;double submittedMs,observedMs,beginCallMs,retireWaitMs;DWORD bytes,error;};
    HANDLE device;
    Clock::time_point origin{};
    unsigned attempts{},completed{};
    std::vector<Completion> records;
    explicit OverlappedAudioWriter(HANDLE value):device(value){records.reserve(10000);}
    void Record(const Token& token,double waitMs=0) noexcept {
        if(records.size()<records.capacity())records.push_back({token.sequence,
            std::chrono::duration<double,std::milli>(token.start-origin).count(),
            std::chrono::duration<double,std::milli>(Clock::now()-origin).count(),token.beginCallMs,waitMs,token.bytes,token.error});
    }
    bool Begin(Token& token,std::span<const std::uint8_t> data) noexcept {
        const auto event=token.request.hEvent;token.request={};token.request.hEvent=event;
        ResetEvent(event);token.bytes=0;token.error=0;token.sequence=attempts++;
        token.start=Clock::now();if(attempts==1)origin=token.start;
        const bool written=WriteFile(device,data.data(),static_cast<DWORD>(data.size()),nullptr,&token.request)!=FALSE;
        const auto error=written?ERROR_SUCCESS:GetLastError();
        token.beginCallMs=std::chrono::duration<double,std::milli>(Clock::now()-token.start).count();
        if(written){
            token.immediate=true;return true;
        }
        token.error=error;
        if(token.error==ERROR_IO_PENDING){token.immediate=false;token.error=0;return true;}
        token.immediate=true;Record(token);return false;
    }
    bool Finish(Token& token,std::size_t wanted) noexcept {
        const auto begin=Clock::now();
        if(!token.immediate){
            const auto status=WaitForSingleObject(token.request.hEvent,1000);
            if(status!=WAIT_OBJECT_0){
                token.error=status==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
                CancelIoEx(device,&token.request);
                DWORD ignored{};GetOverlappedResult(device,&token.request,&ignored,TRUE);
                token.bytes=0;
            }
        }
        if(token.error==0 && !GetOverlappedResult(device,&token.request,&token.bytes,FALSE)){
            token.error=GetLastError();token.bytes=0;
        }
        const bool ok=token.error==0 && token.bytes==wanted;
        if(ok)++completed;
        Record(token,std::chrono::duration<double,std::milli>(Clock::now()-begin).count());return ok;
    }
};
struct PipelinedTransport final:Transport {
    struct ControlWrite {Bytes bytes;int result;};
    WinTransport io;
    OverlappedAudioWriter writer;
    WiimoteAudioWriteQueue<OverlappedAudioWriter> queue;
    std::vector<ControlWrite> controls;
    explicit PipelinedTransport(const std::string& path):io(path),writer(io.device),queue(writer){}
    int Write(std::span<const std::uint8_t> data)override{
        if(!data.empty() && data[0]==0x18)return queue.Submit(data)?static_cast<int>(data.size()):-1;
        // A barrier drains audio before control; allow best-effort OFF even after errors.
        queue.Drain();const int result=io.Write(data);
        controls.push_back({Bytes(data.begin(),data.end()),result});return result;
    }
    int Read(std::span<std::uint8_t> data,unsigned timeout)override{return io.Read(data,timeout);}
    bool FlushAudio()override{return queue.Drain();}
    void DescribeAudio(std::ostream& log)override{
        log<<"completion_summary attempted="<<writer.attempts<<" completed_full="<<writer.completed<<" pending_limit=2\n";
        log<<"completion,sequence,submitted_ms,completion_observed_ms,submit_call_ms,retire_wait_ms,completed_bytes,win32_error\n";
        for(const auto& record:writer.records)log<<"completion,"<<record.sequence<<','<<record.submittedMs<<','<<record.observedMs<<','<<record.beginCallMs<<','<<record.retireWaitMs<<','<<record.bytes<<','<<record.error<<'\n';
        constexpr char hex[]="0123456789abcdef";
        for(const auto& control:controls){
            log<<"control,result="<<control.result<<",hex=";
            for(const auto byte:control.bytes)log<<hex[byte>>4]<<hex[byte&15];
            log<<'\n';
        }
    }
};

WiimoteSpeakerProtocol::State Control(Transport& io,unsigned command,unsigned volume,
                                     WiimoteSpeakerProtocol::State state,bool stayMuted=false,std::ostream* received=nullptr,unsigned sampleRate=6000){
    if(sampleRate!=6000 && sampleRate!=3000)throw std::runtime_error("Unsupported diagnostic sample rate");
    WiimoteSpeakerProtocol::Operation operation;
    if(!operation.Begin(command,static_cast<std::uint8_t>(volume),state,Clock::now()))throw std::runtime_error("Control rejected");
    unsigned step=0;
    for(const auto& current:operation.Steps()){
        auto packet=current.packet;
        if(stayMuted && packet[0]==0x19)packet[1]|=4;
        if(sampleRate==3000 && packet[0]==0x16 && packet[2]==0xa2 && packet[4]==1 && packet[5]==7){
            const auto divider=12000000u/sampleRate;
            packet[8]=static_cast<std::uint8_t>(divider);packet[9]=static_cast<std::uint8_t>(divider>>8);
        }
        // Only the speaker module and status are touched: no mode/IR/extension writes.
        if(packet[0]==0x16 && packet[2]!=0xa2)throw std::runtime_error("Unsafe non-speaker register");
        if(!CompleteWrite(io.Write(packet),packet.size()))throw std::runtime_error("Incomplete speaker control write");
        operation.Written(operation.generation,step,Clock::now());
        const auto deadline=Clock::now()+600ms;
        while(!operation.Ready(operation.generation,step+1,Clock::now()) && !operation.Result()){
            std::array<std::uint8_t,22> data{};
            const int size=io.Read(data,10);
            if(size<0)throw std::runtime_error("Input failed during speaker initialization");
            if(received && size>0){*received<<"control_input,";LogBytes(*received,std::span(data).first(std::min<std::size_t>(size,data.size())));*received<<'\n';}
            if(size>=5 && data[0]==0x22)operation.Acknowledge(data[3],data[4]);
            if(size>=4 && data[0]==0x20)operation.Status(data[3]);
            operation.Poll(Clock::now());
            if(Clock::now()>deadline)throw std::runtime_error("Speaker control ACK/status timeout");
        }
        if(operation.Result() && *operation.Result()!=0)throw std::runtime_error("Speaker returned control error");
        ++step;
    }
    if(!operation.Result() || *operation.Result()!=0)throw std::runtime_error("Speaker control did not complete");
    auto final=operation.FinalState();if(stayMuted)final.muted=true;
    return final;
}

struct ConfigurationReply {
    std::array<std::uint8_t,7> data{};
    std::array<bool,7> present{};
    unsigned error{};
    bool malformed{};
    bool Receive(std::span<const std::uint8_t> report){
        if(report.empty() || report[0]!=0x21)return false;
        if(report.size()<6){malformed=true;return true;}
        const unsigned address=(unsigned(report[4])<<8)|report[5];
        if(address<1 || address>7)return false;
        error=report[3]&15;
        if(error)return true;
        const unsigned count=(report[3]>>4)+1,offset=address-1;
        if(count>data.size()-offset || report.size()<6+count){malformed=true;return true;}
        for(unsigned i=0;i<count;++i){
            if(present[offset+i] && data[offset+i]!=report[6+i]){malformed=true;return true;}
            data[offset+i]=report[6+i];present[offset+i]=true;
        }
        return true;
    }
    bool Complete()const{return !error && !malformed && std::all_of(present.begin(),present.end(),[](bool value){return value;});}
    bool UnavailablePattern()const{
        return Complete() && std::all_of(data.begin(),data.begin()+6,[](auto value){return value==0xff;});
    }
};
void Readback(Transport& io,std::ofstream& log,unsigned volume){
    auto state=WiimoteSpeakerProtocol::State{};
    try{
        // No PLAY/audio reports: configure with the amplifier muted throughout.
        state=Control(io,1,volume,state,true,&log);
        const std::array<std::uint8_t,7> request{0x17,0x04,0xa2,0,1,0,7};
        if(!CompleteWrite(io.Write(request),request.size()))throw std::runtime_error("Incomplete configuration read request");
        ConfigurationReply reply;
        unsigned unrelated=0;
        const auto deadline=Clock::now()+600ms;
        while(!reply.Complete() && !reply.error && !reply.malformed && Clock::now()<deadline){
            std::array<std::uint8_t,22> data{};
            const int size=io.Read(data,10);
            if(size<0)throw std::runtime_error("Input failed during configuration readback");
            if(!size)continue;
            const auto report=std::span(data).first(std::min<std::size_t>(size,data.size()));
            log<<"readback_input,";LogBytes(log,report);log<<'\n';
            if(!reply.Receive(report))++unrelated;
        }
        const auto expected=WiimoteSpeaker::Configuration(static_cast<std::uint8_t>(volume));
        log<<"expected_config,";LogBytes(log,expected);log<<"\nunrelated_reports,"<<unrelated<<'\n';
        if(reply.UnavailablePattern()){
            log<<"actual_config,";LogBytes(log,reply.data);
            log<<"\nreadback_result,inconclusive_ff_open_bus_pattern\n";
            std::cout<<"Readback is mostly FF, not a usable configuration echo. No configuration fault is proven.\n";
        }else if(reply.Complete()){
            const bool matches=std::equal(reply.data.begin(),reply.data.end(),expected.begin());
            log<<"actual_config,";LogBytes(log,reply.data);
            log<<"\nreadback_result,"<<(matches?"matches":"mismatch")<<'\n';
            std::cout<<(matches?"Configuration readback matches. This does NOT prove audio-packet reception.\n":
                "Configuration differs; preserve the log. No automatic correction was applied.\n");
        }else{
            log<<"readback_result,inconclusive,error="<<reply.error<<",malformed="<<reply.malformed<<'\n';
            std::cout<<"Configuration readback is inconclusive (unavailable/error/timeout), NOT a confirmed mismatch.\n";
        }
        state=Control(io,0,volume,state,true,&log);
        io.DescribeAudio(log);log.flush();
    }catch(...){try{Control(io,0,volume,state,true,&log);}catch(...){}throw;}
}

std::vector<Bytes> Load(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);
    const Bytes all((std::istreambuf_iterator<char>(file)),{});
    if(all.size()<8 || std::string(all.begin(),all.begin()+4)!="WRP1")throw std::runtime_error("Bad replay header");
    std::uint32_t count=0;for(unsigned i=0;i<4;++i)count|=std::uint32_t(all[4+i])<<(8*i);
    if(!count || count>10000 || all.size()!=8+std::size_t(count)*20)throw std::runtime_error("Bad replay packet count");
    std::vector<Bytes> packets;
    for(unsigned i=0;i<count;++i)packets.emplace_back(all.begin()+8+i*20,all.begin()+8+(i+1)*20);
    return packets;
}
struct Observation {double startMs,writeMs;int result;};
class ReplayClock {
    WiimoteOutputSchedule::AudioClock normal;
    unsigned rate;
    Clock::time_point nominal,ready;
public:
    ReplayClock(Clock::time_point start,unsigned value):normal(start),rate(value),nominal(start),ready(start){
        if(rate!=6000 && rate!=3000)throw std::runtime_error("Unsupported replay rate");
    }
    Clock::time_point Deadline()const{return rate==6000?normal.Deadline():ready;}
    void Sent(Clock::time_point submission,unsigned bytes){
        if(rate==6000){normal.Sent(submission,bytes);return;}
        const auto period=std::chrono::nanoseconds(std::int64_t(bytes)*2*1000000000/rate);
        if(submission>nominal+20ms)nominal=submission;
        nominal+=period;
        ready=std::max(nominal,submission+5ms);
    }
};
std::vector<Bytes> NearSilence(){
    padscore::YamahaAdpcm codec;
    std::vector<Bytes> packets(450,Bytes(20));
    for(auto& packet:packets)for(auto& byte:packet){
        const auto high=codec.Encode(0),low=codec.Encode(0);
        byte=static_cast<std::uint8_t>((high<<4)|low);
    }
    if(codec.predictor!=0 || codec.step!=127)throw std::runtime_error("Silence encoder did not retain its expected state");
    return packets;
}
void PumpIdle(Transport& io,std::chrono::milliseconds duration){
    const auto end=Clock::now()+duration;
    std::array<std::uint8_t,22> input{};
    while(Clock::now()<end)if(io.Read(input,10)<0)throw std::runtime_error("Input failed during idle isolation stage");
}
void Play(Transport& io,const std::vector<Bytes>& packets,std::ofstream& log,const std::string& label,unsigned volume,unsigned sampleRate=6000){
    WiimoteSpeakerProtocol::State state;
    try{
        state=Control(io,1,volume,state,false,nullptr,sampleRate);
        state=Control(io,4,volume,state,false,nullptr,sampleRate);
        std::atomic_bool readFailed=false;
        std::jthread input([&](std::stop_token stop){
            std::array<std::uint8_t,22> data{};
            while(!stop.stop_requested()){if(io.Read(data,10)<0){readFailed=true;break;}}
        });
        std::vector<Observation> observations;observations.reserve(packets.size());
        WiimoteOutputWait wait;
        const auto begin=Clock::now();
        ReplayClock clock(begin,sampleRate);
        bool failed=false;
        for(std::size_t i=0;i<packets.size();++i){
            wait.WaitUntil(clock.Deadline(),true);
            const auto report=WiimoteSpeaker::Data(packets[i]);
            const auto start=Clock::now();
            const int count=io.Write(report);
            const auto end=Clock::now();
            clock.Sent(start,static_cast<unsigned>(packets[i].size()));
            observations.push_back({std::chrono::duration<double,std::milli>(start-begin).count(),
                std::chrono::duration<double,std::milli>(end-start).count(),count});
            if(!CompleteWrite(count,report.size()) || readFailed){failed=true;break;}
        }
        failed=!io.FlushAudio() || failed;
        wait.WaitUntil(clock.Deadline()+100ms,true);
        input.request_stop();input.join();
        failed=failed || readFailed.load();
        state=Control(io,0,volume,state);
        // Disk output occurs only after playback and speaker shutdown.
        log<<(failed?"IO_FAILED ":"IO_COMPLETE ")<<label<<" volume="<<volume<<" sample_rate="<<sampleRate<<" packets="<<observations.size()<<" read_failed="<<readFailed.load()<<"\n";
        log<<"start_ms,write_or_enqueue_ms,accepted_bytes\n";
        for(const auto& sample:observations)log<<sample.startMs<<','<<sample.writeMs<<','<<sample.result<<'\n';
        io.DescribeAudio(log);
        log.flush();
        if(failed)throw std::runtime_error("Audio I/O failed; see comparison log");
    }catch(...){try{Control(io,0,volume,state);}catch(...){}throw;}
}
void Isolate(Transport& io,const std::vector<Bytes>& effects,std::ofstream& log,unsigned volume){
    auto state=WiimoteSpeakerProtocol::State{};
    try{
        std::cout<<"1/4: amplifier MUTED, no audio data, 1.5 seconds. Expected quiet.\n"<<std::flush;
        state=Control(io,1,volume,state);state=Control(io,2,volume,state);state=Control(io,4,volume,state);
        log<<"isolation_stage,muted_no_data,1500ms\n";PumpIdle(io,1500ms);
        std::cout<<"2/4: amplifier ENABLED, no audio data, 1.5 seconds. Expected quiet.\n"<<std::flush;
        state=Control(io,3,volume,state);
        log<<"isolation_stage,unmuted_no_data,1500ms\n";PumpIdle(io,1500ms);
        state=Control(io,0,volume,state);
        // Each Play resets the physical decoder before its own continuous stream.
        std::cout<<"3/4: encoded NEAR-SILENCE, 3 seconds. No Mario effects in this section.\n"<<std::flush;
        log<<"isolation_stage,encoded_near_silence,450packets\n";
        Play(io,NearSilence(),log,"ISOLATION_NEAR_SILENCE",volume);
        std::cout<<"4/4: ORIGINAL EFFECTS, about 2.6 seconds. Listen for the overlay.\n"<<std::flush;
        log<<"isolation_stage,original_effects,392packets\n";
        Play(io,effects,log,"ISOLATION_EFFECTS",volume);
    }catch(...){try{Control(io,0,volume,state);}catch(...){}throw;}
}

struct FakeTransport:Transport {
    std::vector<Bytes> writes;
    Bytes response;
    bool enabled=false;
    int Write(std::span<const std::uint8_t> data)override{
        writes.emplace_back(data.begin(),data.end());
        if(data[0]==0x14)enabled=(data[1]&4)!=0;
        response=data[0]==0x15?Bytes{0x20,0,0,static_cast<std::uint8_t>(enabled?4:0),0,0,0xc0}:
            Bytes{0x22,0,0,data[0],0};
        return static_cast<int>(data.size());
    }
    int Read(std::span<std::uint8_t> data,unsigned)override{
        std::copy(response.begin(),response.end(),data.begin());const auto count=response.size();response.clear();return static_cast<int>(count);
    }
};
void SelfTest(const std::filesystem::path& path){
    WiimoteAudioWriteQueueTests::Run();
    const auto silence=NearSilence();assert(silence.size()==450);
    for(const auto& packet:silence)for(auto byte:packet)assert(byte==0x08);
    auto packets=Load(path);assert(!packets.empty());
    FakeTransport io;
    auto state=Control(io,1,127,{});assert(state.enabled && !state.playing);
    assert(!state.muted && io.writes[5]==Bytes({0x19,2}));
    state=Control(io,4,127,state);assert(state.playing);
    state=Control(io,0,127,state);assert(!state.enabled);
    for(const auto& packet:io.writes){
        assert(packet[0]==0x14 || packet[0]==0x19 || packet[0]==0x16 || packet[0]==0x15);
        if(packet[0]==0x16)assert(packet[2]==0xa2);
    }
    assert(io.writes[3][4]==1 && io.writes[3][6]==0x80);
    assert(!CompleteWrite(0,22) && !CompleteWrite(-1,22) && !CompleteWrite(21,22));
    assert(CompleteWrite(22,22) && CompleteWrite(22,2));
    ConfigurationReply reply;
    assert(!reply.Receive(Bytes{0x30,0,0}));
    assert(reply.Receive(Bytes{0x21,0,0,0x20,0,1,0,0,0xd0}) && !reply.Complete());
    assert(reply.Receive(Bytes{0x21,0,0,0x30,0,4,7,127,12,14}) && reply.Complete());
    assert((reply.data==std::array<std::uint8_t,7>({0,0,0xd0,7,127,12,14})));
    ConfigurationReply unavailable;assert(unavailable.Receive(Bytes{0x21,0,0,7,0,1}) && unavailable.error==7 && !unavailable.Complete());
    ConfigurationReply truncated;assert(truncated.Receive(Bytes{0x21,0,0,0x60,0,1,0}) && truncated.malformed && !truncated.Complete());
    ConfigurationReply range;assert(!range.Receive(Bytes{0x21,0,0,0,0,0x20,0}) && !range.Complete());
    assert(range.Receive(Bytes{0x21,0,0,0x10,0,7,0,0}) && range.malformed);
    ConfigurationReply duplicate;assert(duplicate.Receive(Bytes{0x21,0,0,0,0,1,0}));
    assert(duplicate.Receive(Bytes{0x21,0,0,0,0,1,1}) && duplicate.malformed);
    ConfigurationReply unavailableEcho;
    assert(unavailableEcho.Receive(Bytes{0x21,0,0,0x60,0,1,255,255,255,255,255,255,1}) && unavailableEcho.UnavailablePattern());
    assert(!reply.UnavailablePattern());
    FakeTransport muted;
    const auto mutedState=Control(muted,1,127,{},true);assert(mutedState.muted);
    for(const auto& packet:muted.writes){
        assert(packet[0]!=0x18 && packet[0]!=0x12);
        if(packet[0]==0x19)assert((packet[1]&4)!=0);
        if(packet[0]==0x16)assert(packet[2]==0xa2 && packet[4]!=8);
    }
    FakeTransport lowerRate;
    Control(lowerRate,1,127,{},false,nullptr,3000);
    assert(lowerRate.writes[4][8]==0xa0 && lowerRate.writes[4][9]==0x0f);
    assert(lowerRate.writes[4][7]==0 && lowerRate.writes[4][10]==127);
    assert(lowerRate.writes.size()==7);
    for(unsigned i=0;i<7;++i){
        auto expected=io.writes[i];
        if(i==4){expected[8]=0xa0;expected[9]=0x0f;}
        assert(lowerRate.writes[i]==expected);
    }
    const auto start=Clock::time_point{}+1s;
    ReplayClock normalClock(start,6000),lowClock(start,3000);
    for(unsigned i=0;i<196;++i){normalClock.Sent(normalClock.Deadline(),20);lowClock.Sent(lowClock.Deadline(),20);}
    assert(lowClock.Deadline()-start==13333333ns*196);
    assert(normalClock.Deadline()-start==6666666ns*196);
    std::cout<<"PASS FF readback is inconclusive; reduced-bandwidth divider, unchanged format/volume and independent audio-only cadence. No hardware.\n";
    std::cout<<"PASS configuration reply assembly, unavailable/error, truncation, wrong address, duplicate conflict and muted-only initialization. No hardware.\n";
    std::cout<<"PASS bounded two-write queue, stable buffers, ordered drain, failed/short completion handling, exception cleanup, replay/controls. No HID enumeration/device access.\n";
}
void RequireClosedEmulators(){
    const HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot verify closed emulators");
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    bool blocked=false;
    if(Process32FirstW(snapshot,&entry))do{
        blocked|=_wcsicmp(entry.szExeFile,L"Cemu.exe")==0 ||
            _wcsicmp(entry.szExeFile,L"Cemu_release.exe")==0 || _wcsicmp(entry.szExeFile,L"Dolphin.exe")==0;
    }while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    if(blocked)throw std::runtime_error("Close Cemu/Dolphin normally before the comparison; nothing was stopped");
}
void TimingModel(const std::filesystem::path& path){
    std::ifstream file(path);
    if(!file)throw std::runtime_error("Cannot open existing timing log");
    const auto base=Clock::time_point{}+1s;
    auto legacy=base,legacyEnd=base,newEnd=base,lastNew=base,lastOld=base;
    WiimoteOutputSchedule::AudioClock clock(base);
    unsigned packets=0,stalls=0;
    auto smallest=Clock::duration::max();
    auto legacyNext=[](Clock::time_point deadline,Clock::time_point sent){
        if(sent>deadline+20ms)deadline=sent;
        return std::max(deadline+6666666ns,sent+5ms);
    };
    for(std::string line;std::getline(file,line);){
        if(line.empty() || line[0]<'0' || line[0]>'9')continue;
        std::istringstream row(line);double startMs,writeMs;int count;char comma1,comma2;
        if(!(row>>startMs>>comma1>>writeMs>>comma2>>count) || comma1!=',' || comma2!=',' || count!=22)
            throw std::runtime_error("Bad timing row or incomplete write");
        const auto observed=base+std::chrono::nanoseconds(static_cast<std::int64_t>(startMs*1000000));
        const auto latency=std::chrono::nanoseconds(static_cast<std::int64_t>(writeMs*1000000));
        const auto overhead=std::max(Clock::duration{},observed-std::max(legacy,legacyEnd));
        const auto sent=std::max(clock.Deadline(),newEnd)+overhead;
        if(packets)smallest=std::min(smallest,sent-lastNew);
        clock.Sent(sent,20);newEnd=sent+latency;lastNew=sent;lastOld=observed;
        legacy=legacyNext(legacy,observed);legacyEnd=observed+latency;
        ++packets;if(latency>10ms)++stalls;
    }
    if(!packets)throw std::runtime_error("No timing records");
    const auto ms=[](auto duration){return std::chrono::duration<double,std::milli>(duration).count();};
    std::cout<<"{\"kind\":\"model_not_hardware\",\"packets\":"<<packets
        <<",\"observed_stalls\":"<<stalls<<",\"nominal_ms\":"<<packets*40.0/6.0
        <<",\"old_ms\":"<<ms(lastOld-base+6666666ns)
        <<",\"phase_recovery_ms\":"<<ms(lastNew-base+6666666ns)
        <<",\"minimum_submission_gap_ms\":"<<(packets>1?ms(smallest):0.0)
        <<",\"assumption\":\"same per-packet write durations and host wake overhead; physical stalls are not removed\"}\n";
}
int main(int argc,char** argv){
    try{
        if(argc==3 && std::string(argv[1])=="--self-test"){SelfTest(argv[2]);return 0;}
        if(argc==3 && std::string(argv[1])=="--timing-model"){TimingModel(argv[2]);return 0;}
        const bool isolationCheck=argc==4 && std::string(argv[1])=="--isolation-check";
        const bool readbackCheck=argc==4 && std::string(argv[1])=="--readback-check";
        const bool bandwidthCheck=argc==4 && std::string(argv[1])=="--bandwidth-check";
        const bool sequenceCheck=argc==4 && std::string(argv[1])=="--sdk-sequence-check";
        const bool queuedCheck=argc==4 && std::string(argv[1])=="--queued-check";
        const bool phaseCheck=argc==4 && std::string(argv[1])=="--phase-check";
        const bool sdkCheck=argc==4 && std::string(argv[1])=="--sdk-check";
        if(argc!=4 || (!bandwidthCheck && !readbackCheck && !isolationCheck && !sequenceCheck && !queuedCheck && !sdkCheck && !phaseCheck && std::string(argv[1])!="--hardware"))throw std::runtime_error("Use the reviewed launcher; hardware replay requires an explicit mode");
        RequireClosedEmulators();
        const auto packets=Load(argv[2]);
        const auto current=static_cast<unsigned>(std::stoul(argv[3]));
        if(current>127)throw std::runtime_error("Invalid saved speaker volume");
        std::cout<<"Cemu and Dolphin must both be closed. Only the speaker module will be configured.\n";
        if(bandwidthCheck)std::cout<<"Reduced-bandwidth diagnostic ONLY: 3000 Hz ADPCM, 75 packets/second instead of150.\n"
            <<"Same cues at normal pitch/duration, unchanged volume; reduced high-frequency detail. NOT a full-quality fix.\n"
            <<"No input/report-mode/MotionPlus throttling, no Cemu or driver change.\n";
        else if(readbackCheck)std::cout<<"Muted configuration readback ONLY, about one second. No audio data or PLAY command.\n"
            <<"An unavailable register is inconclusive, not proof of a fault. No MotionPlus/extension/report-mode writes.\n";
        else if(isolationCheck)std::cout<<"Diagnostic isolation ONLY: muted, enabled/no data, encoded near-silence, original effects.\n"
            <<"This is not another claimed fix. About 9 seconds, no settings changes or recording.\n";
        else if(sequenceCheck)std::cout<<"One pass: native Cafe SDK ON/PLAY sequence, using the same queued delivery.\n"
            <<"No decoder-stop write during ON; PLAY starts decoding without changing amplifier mute.\n";
        else if(queuedCheck)std::cout<<"One pass: Windows WriteFile with at most two pending audio writes.\n"
            <<"Same captured audio, rate, volume, controls and input reads. Completion is checked before reporting success.\n";
        else if(phaseCheck)std::cout<<"One pass: SDL with sample-clock recovery across periodic HID write stalls.\n"
            <<"Audio/rate/volume/controls unchanged; minimum packet spacing remains 5 ms.\n";
        else if(sdkCheck)std::cout<<"One pass: SDL with corrected Cafe SDK initialization (register 1 = 0x80).\n"
            <<"The captured audio, saved volume, sample rate and packet timing are unchanged.\n";
        else std::cout<<"Three short passes replay the exact same captured ADPCM data; no background scanning.\n"
            <<"A: SDL at saved volume. B: native Windows at saved volume. C: native Windows at temporary volume 64.\n";
        std::cout<<"No profiles/settings/saves are written. Press Enter to begin device selection, or close now.\n";
        std::string answer;std::getline(std::cin,answer);
        if(SDL_hid_init()!=0)throw std::runtime_error("SDL HID initialization failed");
        std::vector<std::string> paths;
        auto devices=SDL_hid_enumerate(0x057e,0);
        for(auto entry=devices;entry;entry=entry->next){
            if(entry->product_id!=0x0306 && entry->product_id!=0x0330)continue;
            if(entry->product_string && std::wstring(entry->product_string)==L"Nintendo RVL-CNT-01-UC")continue;
            if(entry->path)paths.emplace_back(entry->path);
        }
        SDL_hid_free_enumeration(devices);
        std::string selected;
        for(const auto& path:paths){
            try{
                SdlTransport candidate(path);
                const std::array<std::uint8_t,2> probe{0x15,0};
                if(!CompleteWrite(candidate.Write(probe),probe.size()))continue;
                selected=path;break;
            }catch(const std::exception&){}
        }
        if(selected.empty())throw std::runtime_error("No live Wiimote found. Turn on one remote with DolphinBar in mode 4 and rerun.");
        std::ofstream log("wire-comparison.log",std::ios::out|std::ios::trunc);
        if(!log)throw std::runtime_error("Cannot open comparison log");
        if(bandwidthCheck){
            {PipelinedTransport io(selected);Play(io,packets,log,"DOLPHINBAR_REDUCED_BANDWIDTH_DIAGNOSTIC",current,3000);}
            SDL_hid_exit();
            std::cout<<"Finished. Report whether wah-wah/crackle remains. This does not establish full-quality support.\n";
            return 0;
        }
        if(readbackCheck){
            {PipelinedTransport io(selected);Readback(io,log,current);}
            SDL_hid_exit();
            std::cout<<"Finished. Reply done; no listening test or game replay is needed.\n";
            return 0;
        }
        if(isolationCheck){
            {PipelinedTransport io(selected);Isolate(io,packets,log,current);}
            SDL_hid_exit();
            std::cout<<"Finished. Which stage first produced the unwanted wah-wah? Report1/2/3/4.\n";
            return 0;
        }
        if(queuedCheck || sequenceCheck){
            std::cout<<(sequenceCheck?"SDK sequence check, saved volume ":"Queued audio check, saved volume ")<<current<<". Listen now.\n"<<std::flush;
            {PipelinedTransport io(selected);Play(io,packets,log,sequenceCheck?"SDK_SEQUENCE_QUEUED":"QUEUED_2_NATIVE",current);}
            SDL_hid_exit();
            std::cout<<"Finished. Report whether the wah-wah overlay is gone or still masks the effects.\n";
            return 0;
        }
        if(sdkCheck || phaseCheck){
            std::cout<<(phaseCheck?"Phase recovery check: SDL, saved volume ":"SDK check: SDL, saved volume ")<<current<<". Listen now.\n"<<std::flush;
            {SdlTransport io(selected);Play(io,packets,log,phaseCheck?"PHASE_RECOVERY_SDL":"SDK_80_SDL",current);}
            SDL_hid_exit();
            std::cout<<"Finished. Report whether the captured cues are recognizable/clear or still pulsing/crackling.\n";
            return 0;
        }
        std::cout<<"A: SDL, saved volume "<<current<<". Listen now.\n"<<std::flush;
        {SdlTransport io(selected);Play(io,packets,log,"A_SDL",current);}
        std::cout<<"A finished. Press Enter for B: native Windows, same volume.\n";std::getline(std::cin,answer);
        std::cout<<"B: native Windows, saved volume "<<current<<". Listen now.\n"<<std::flush;
        {WinTransport io(selected);Play(io,packets,log,"B_NATIVE",current);}
        std::cout<<"B finished. Press Enter for C: native Windows, temporary volume 64.\n";std::getline(std::cin,answer);
        std::cout<<"C: native Windows, temporary volume 64. Listen now.\n"<<std::flush;
        {WinTransport io(selected);Play(io,packets,log,"C_NATIVE_64",64);}
        SDL_hid_exit();
        std::cout<<"Finished. Report A/B/C clarity; log is saved. No emulator profile was changed.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"ERROR: "<<error.what()<<'\n';return 1;}
}
