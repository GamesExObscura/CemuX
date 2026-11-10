#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
void Check(HRESULT result,const char* operation){
    if(FAILED(result)){std::cerr<<operation<<" HRESULT=0x"<<std::hex<<static_cast<unsigned>(result)<<'\n';throw std::runtime_error(operation);}
}
struct Runtime {
    bool com{},mf{};
    Runtime(){Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM startup");com=true;
        Check(MFStartup(MF_VERSION,MFSTARTUP_LITE),"Media Foundation startup");mf=true;}
    ~Runtime(){if(mf)MFShutdown();if(com)CoUninitialize();}
};
struct Output {
    HANDLE value;
    explicit Output(const wchar_t* path):value(CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)){
        if(value==INVALID_HANDLE_VALUE)throw std::runtime_error("Output already exists or cannot be created");
    }
    ~Output(){CloseHandle(value);}
    void Write(const void* data,DWORD size){DWORD done{};
        if(!WriteFile(value,data,size,&done,nullptr) || done!=size)throw std::runtime_error("Incomplete WAV write");}
};
int wmain(int argc,wchar_t** argv){
    try{
        if(argc!=3 || !std::filesystem::is_regular_file(argv[1]))throw std::runtime_error("Use input-local-file output-new.wav; no capture or playback");
        Runtime runtime;
        ComPtr<IMFSourceReader> reader;
        Check(MFCreateSourceReaderFromURL(argv[1],nullptr,&reader),"Open local audio file");
        Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"Deselect streams");
        Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE),"Select audio");
        ComPtr<IMFMediaType> type;
        Check(MFCreateMediaType(&type),"Create PCM type");
        Check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),"Set audio type");
        Check(type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM),"Set PCM subtype");
        Check(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16),"Set PCM bits");
        Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,nullptr,type.Get()),"Decode to PCM");
        Check(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,&type),"Get decoded format");
        UINT32 rate{},channels{},bits{};
        Check(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,&rate),"Get rate");
        Check(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,&channels),"Get channels");
        Check(type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,&bits),"Get bits");
        if(bits!=16 || !rate || rate>192000 || !channels || channels>8)throw std::runtime_error("Unsupported decoded format");
        const auto block=channels*2;
        std::vector<std::uint8_t> pcm;
        for(;;){
            DWORD flags{};LONGLONG timestamp{};ComPtr<IMFSample> sample;
            Check(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,nullptr,&flags,&timestamp,&sample),"Read decoded sample");
            if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)throw std::runtime_error("Midstream format change");
            if(sample){
                ComPtr<IMFMediaBuffer> buffer;Check(sample->ConvertToContiguousBuffer(&buffer),"Get PCM buffer");
                BYTE* data{};DWORD size{};Check(buffer->Lock(&data,nullptr,&size),"Lock PCM buffer");
                if(pcm.size()+size>std::uint64_t(rate)*block*120 || size%block){buffer->Unlock();throw std::runtime_error("Recording exceeds120s or has partial frames");}
                pcm.insert(pcm.end(),data,data+size);Check(buffer->Unlock(),"Unlock PCM buffer");
            }
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
        }
        if(pcm.empty())throw std::runtime_error("No decoded audio");
        std::array<std::uint8_t,44> header{};
        auto word=[&](unsigned offset,std::uint32_t value,unsigned count){for(unsigned i=0;i<count;++i)header[offset+i]=static_cast<std::uint8_t>(value>>(8*i));};
        const char riff[]="RIFF",wave[]="WAVE",fmt[]="fmt ",data[]="data";
        std::copy_n(riff,4,header.begin());std::copy_n(wave,4,header.begin()+8);
        std::copy_n(fmt,4,header.begin()+12);std::copy_n(data,4,header.begin()+36);
        word(4,36+static_cast<std::uint32_t>(pcm.size()),4);word(16,16,4);word(20,1,2);
        word(22,channels,2);word(24,rate,4);word(28,rate*block,4);word(32,block,2);word(34,16,2);
        word(40,static_cast<std::uint32_t>(pcm.size()),4);
        Output out(argv[2]);out.Write(header.data(),44);out.Write(pcm.data(),static_cast<DWORD>(pcm.size()));
        std::cout<<"{\"status\":\"decoded_file_only\",\"rate\":"<<rate<<",\"channels\":"<<channels
            <<",\"duration_seconds\":"<<double(pcm.size())/rate/block<<",\"audio_capture_or_playback\":false}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"ERROR: "<<error.what()<<'\n';return 1;}
}
