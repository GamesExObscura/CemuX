#include "Cafe/OS/common/OSCommon.h"
#include "nn_uds.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/Account/Account.h"
#include "Cafe/CafeSystem.h"
#include "UdsHost.h"
#include <openssl/evp.h>
#include <openssl/rand.h>

typedef struct  
{
	uint32 reserved;
}udsWorkspace_t;

udsWorkspace_t* udsWorkspace = nullptr;

void nnUdsExport___sti___11_uds_Api_cpp_f5d9abb2(PPCInterpreter_t* hCPU)
{
	debug_printf("__sti___11_uds_Api_cpp_f5d9abb2()\n");
	if( udsWorkspace == NULL )
		udsWorkspace = (udsWorkspace_t*)memory_getPointerFromVirtualOffset(coreinit_allocFromSysArea(32, 32));
	osLib_returnFromFunction(hCPU, memory_getVirtualOffsetFromPointer(udsWorkspace));
}

namespace nn::uds
{
	namespace
	{
		using namespace WiiU3DSLink;
		struct SchedulerGuard {
			SchedulerGuard() { __OSLockScheduler(); }
			~SchedulerGuard() { __OSUnlockScheduler(); }
		};
		struct GuestStatus {
			uint32be state, reason;
			uint16be id, changed;
			uint16be nodes[16];
			uint8 count, maximum;
			uint16be mask;
		};
		struct GuestNode {
			uint64be friendCode;
			uint16be friendNode, xorKey;
			uint16be name[12];
			uint16be id, reserved;
		};
		static_assert(sizeof(GuestStatus)==0x30);
		static_assert(sizeof(GuestNode)==0x28 && offsetof(GuestNode,id)==0x24);
		std::unique_ptr<UdsHost> host;
		coreinit::OSHostAlarm* alarm{};
		SysAllocator<coreinit::OSEvent,17> events;
		bool peerPresent{};
		uint8 lastClientCount{};
		std::string publishedStatus;

		constexpr uint32 LegacyResult(int level, uint32 summary, uint32 description)
		{
			const int mapped = level==-7 ? -3 : level;
			return ((static_cast<uint32>(mapped)&7)<<29) | (3U<<27) | (68U<<20) |
				((static_cast<uint32>(level)&15)<<14) | (summary<<10) | description;
		}
		uint32 Result(UdsResult value)
		{
			switch(value) {
			case UdsResult::Ok: return 0;
			case UdsResult::NotInitialized: return LegacyResult(-2,5,1016);
			case UdsResult::State: return LegacyResult(-3,5,1002);
			case UdsResult::Argument: return LegacyResult(-2,8,1021);
			case UdsResult::Resource: return LegacyResult(-3,3,1011);
			case UdsResult::NotFound: return LegacyResult(-7,4,1018);
			case UdsResult::TooLarge: return LegacyResult(-2,8,1001);
			case UdsResult::Busy: return LegacyResult(-3,3,1008);
			}
			return LegacyResult(-1,11,7);
		}
		bool Range(const void* data, std::size_t size)
		{
			if(!data) return size==0;
			const auto address=reinterpret_cast<uintptr_t>(data);
			const auto base=reinterpret_cast<uintptr_t>(memory_base);
			return address>=base && address-base<0x100000000ULL && size<=0x100000000ULL-(address-base);
		}
		std::vector<uint8> Digest(std::span<const uint8> bytes,const EVP_MD* algorithm)
		{
			std::vector<uint8> digest(EVP_MAX_MD_SIZE); unsigned count{};
			if(EVP_Digest(bytes.data(),bytes.size(),digest.data(),&count,algorithm,nullptr)!=1) return {};
			digest.resize(count); return digest;
		}
		UdsCrypto Crypto()
		{
			return {
				[](auto bytes){return Digest(bytes,EVP_md5());},
				[](auto bytes){return Digest(bytes,EVP_sha1());},
				[](std::span<const uint8> bytes,const std::array<uint8,16>& counter) {
					// Matches Azahar's emulator-only beacon key; not console radio support.
					const std::array<uint8,16> key{};
					std::vector<uint8> result(bytes.size()+16); int count{}, tail{};
					auto* context=EVP_CIPHER_CTX_new();
					if(!context) return std::vector<uint8>{};
					const bool ok=EVP_EncryptInit_ex(context,EVP_aes_128_ctr(),nullptr,key.data(),counter.data())==1 &&
						EVP_EncryptUpdate(context,result.data(),&count,bytes.data(),static_cast<int>(bytes.size()))==1 &&
						EVP_EncryptFinal_ex(context,result.data()+count,&tail)==1;
					EVP_CIPHER_CTX_free(context);
					if(!ok) return std::vector<uint8>{};
					result.resize(count+tail); return result;
				}
			};
		}
		void Signal()
		{
			if(!host || !host->Initialized()) return;
			if(host->HasStateChange()) coreinit::OSSignalEventInternal(events.GetPtr());
			for(std::size_t i=0;i<16;++i)
				if(host->HasPacket(i)) coreinit::OSSignalEventInternal(events.GetPtr()+i+1);
		}
		void Pump(uint64 tick,void*)
		{
			if(!host || !host->Initialized()) return;
			auto& transport=GetTransport(); const auto link=transport.GetStatus();
			if(!link.enabled && host->Status().state==6) host->DestroyNetwork(3);
			if(peerPresent && !link.peer_present) host->PeerLost();
			peerPresent=link.peer_present;
			for(unsigned i=0;i<128;++i) { auto frame=transport.Receive(); if(!frame) break; host->Process(*frame); }
			if(link.enabled) {
				const uint64 clock=coreinit::EspressoTime::GetTimerClock();
				const uint64 microseconds=(tick/clock)*1000000ULL+(tick%clock)*1000000ULL/clock;
				try {
					if(auto beacon=host->Beacon(microseconds)) transport.Send(*beacon);
				} catch(const std::exception& error) {
					cemuLog_log(LogType::Force,"[WiiU3DS] Beacon preparation failed: {}",error.what());
					host->DestroyNetwork(3);
				}
			}
			while(auto frame=host->PopOutgoing()) transport.Send(*frame);
			Signal();
			const auto clients=static_cast<uint8>(host->Status().count ? host->Status().count-1 : 0);
			if(clients!=lastClientCount) {
				cemuLog_log(LogType::Force,"[WiiU3DS] Native UDS clients: {}",clients);
				lastClientCount=clients;
			}
			auto description=fmt::format("Wii U UDS state {}; 3DS clients {}; application packets received {} / sent {}",
				host->Status().state,clients,host->Received(),host->Transmitted());
			if(description!=publishedStatus) {publishedStatus=description; transport.SetGuestStatus(std::move(description));}
		}
		uint32 InitializeUser(void* memory,uint32 size,const uint16be* name)
		{
			SchedulerGuard guard;
			if(!GetTransport().GetStatus().enabled) return Result(UdsResult::State);
			if(!Range(memory,size) || (reinterpret_cast<uintptr_t>(memory)&63) || (name && !Range(name,24))) return Result(UdsResult::Argument);
			if(!host) host=std::make_unique<UdsHost>(Crypto());
			const auto& account=Account::GetCurrentAccount(); UdsNode identity;
			identity.friend_code=account.GetTransferableIdBase();
			if(!identity.friend_code) identity.friend_code=account.GetPersistentId();
			identity.mac={2,0x43,0x45,0x4D,0x55,1};
			const auto accountName=account.GetMiiName();
			for(std::size_t i=0;i<identity.name.size();++i)
				identity.name[i]=name ? static_cast<uint16>(name[i]) : (i<accountName.size()?static_cast<uint16>(accountName[i]):0);
			const auto result=host->Initialize(identity,size);
			if(result!=UdsResult::Ok) return Result(result);
			for(unsigned i=0;i<17;++i) coreinit::OSInitEvent(events.GetPtr()+i,coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED,coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			const auto period=coreinit::EspressoTime::ConvertMsToTimerTicks(5);
			alarm=coreinit::OSHostAlarmCreate(coreinit::OSGetTime()+period,period,Pump,nullptr);
			peerPresent=false; lastClientCount=0; publishedStatus.clear();
			cemuLog_log(LogType::Force,"[WiiU3DS] Native UDS initialized; workspace {} bytes",size);
			return 0;
		}
		uint32 Initialize(void* memory,uint32 size) {return InitializeUser(memory,size,nullptr);}
		void Finalize()
		{
			if(!host || !host->Initialized()) return;
			SchedulerGuard guard;
			if(alarm) {coreinit::OSHostAlarmDestroy(alarm); alarm=nullptr;}
			if(host->Status().state==6) host->DestroyNetwork();
			while(auto frame=host->PopOutgoing()) GetTransport().Send(*frame);
			cemuLog_log(LogType::Force,"[WiiU3DS] Native UDS finalized; application packets received {} / sent {}; rejected frames {}",host->Received(),host->Transmitted(),host->Rejected());
			const auto transport=GetTransport().GetStatus();
			cemuLog_log(LogType::Force,"[WiiU3DS] Transport totals received {} / sent {}; dropped {} / rejected {}",transport.received,transport.transmitted,transport.dropped,transport.rejected);
			host->Finalize();
			for(unsigned i=0;i<17;++i) coreinit::OSSignalEventAllInternal(events.GetPtr()+i);
			GetTransport().SetGuestStatus("Wii U native UDS finalized.");
		}
		uint32 CreateCommunicationId(uint32 uniqueId,bool demo) {return ((uniqueId&0xFFFFF)<<8) | (demo?0x11U:0x10U);}
		uint32 CreateNetwork(uint8 id,uint8 maximum,uint32 communicationId,const char* passphrase,uint32 passphraseSize,uint8 channel,const void* app,uint32 appSize)
		{
			SchedulerGuard guard;
			if(!host || !host->Initialized()) return Result(UdsResult::NotInitialized);
			if(passphraseSize<8 || passphraseSize>255 || !Range(passphrase,passphraseSize) || appSize>200 || !Range(app,appSize)) return Result(UdsResult::Argument);
			uint32 networkId{}; if(RAND_bytes(reinterpret_cast<uint8*>(&networkId),sizeof(networkId))!=1) return LegacyResult(-1,11,7);
			if(!networkId) networkId=1;
			const auto result=host->CreateNetwork(id,maximum,communicationId,channel,networkId,{static_cast<const uint8*>(app),appSize});
			cemuLog_log(LogType::Force,"[WiiU3DS] CreateNetwork comm={:08x} id={} maximum={} channel={} result={}",communicationId,id,maximum,channel,static_cast<unsigned>(result));
			Pump(coreinit::OSGetTime(),nullptr); return Result(result);
		}
		uint32 CreateNetworkSimple(uint8 id,uint8 maximum,uint32 comm,const char* pass,uint32 size,uint8 channel) {return CreateNetwork(id,maximum,comm,pass,size,channel,nullptr,0);}
		uint32 CreateNetworkDefault(uint8 id,uint8 maximum,uint32 comm,const char* pass,uint32 size) {return CreateNetworkSimple(id,maximum,comm,pass,size,0);}
		uint32 Allow() {SchedulerGuard guard; return Result(host?host->Allow(true):UdsResult::NotInitialized);}
		uint32 Disallow(bool) {SchedulerGuard guard; return Result(host?host->Allow(false):UdsResult::NotInitialized);}
		uint32 EjectClient(uint16 node) {
			SchedulerGuard guard;
			if(!host) return Result(UdsResult::NotInitialized);
			const auto result=host->Eject(node); Pump(coreinit::OSGetTime(),nullptr);
			return Result(result);
		}
		uint32 SetApplicationData(const void* data,uint32 size) {
			SchedulerGuard guard;
			if(!Range(data,size)) return Result(UdsResult::Argument);
			if(!host) return Result(UdsResult::NotInitialized);
			return Result(host->SetApplicationData({static_cast<const uint8*>(data),size}));
		}
		uint32 DestroyNetwork() {
			SchedulerGuard guard; if(!host) return Result(UdsResult::NotInitialized);
			const auto result=host->DestroyNetwork(); Pump(coreinit::OSGetTime(),nullptr);
			for(unsigned i=1;i<17;++i) coreinit::OSSignalEventAllInternal(events.GetPtr()+i);
			return Result(result);
		}
		uint32 CreateEndpoint(uint32be* fd) {
			SchedulerGuard guard; if(!fd || !Range(fd,4)) return Result(UdsResult::Argument);
			uint32 descriptor{}; const auto result=host?host->CreateEndpoint(descriptor):UdsResult::NotInitialized;
			cemuLog_log(LogType::Force,"[WiiU3DS] CreateEndpoint descriptor={} result={}",descriptor,static_cast<unsigned>(result));
			if(result==UdsResult::Ok) *fd=descriptor; return Result(result);
		}
		uint32 Attach(uint32be* fd,uint16 node,uint8 channel,uint32 size) {
			SchedulerGuard guard; if(!fd || !Range(fd,4)) return Result(UdsResult::Argument);
			const auto result=host?host->Attach(*fd,node,channel,size):UdsResult::NotInitialized;
			cemuLog_log(LogType::Force,"[WiiU3DS] Attach descriptor={} peer={} channel={} capacity={} result={}",static_cast<uint32>(*fd),node,channel,size,static_cast<unsigned>(result));
			return Result(result);
		}
		uint32 DestroyEndpoint(uint32be* fd) {
			SchedulerGuard guard; if(!fd || !Range(fd,4)) return Result(UdsResult::Argument);
			if(!host) return Result(UdsResult::NotInitialized);
			const auto index=host->EndpointIndex(*fd); const auto result=host->DestroyEndpoint(*fd);
			if(result==UdsResult::Ok) {*fd=0; coreinit::OSSignalEventAllInternal(events.GetPtr()+index+1);}
			return Result(result);
		}
		uint32 GetStatus(GuestStatus* output) {
			SchedulerGuard guard; if(!output || !Range(output,sizeof(*output))) return Result(UdsResult::Argument);
			if(!host || !host->Initialized()) return Result(UdsResult::NotInitialized);
			Pump(coreinit::OSGetTime(),nullptr); const auto status=host->TakeStatus(); *output={};
			output->state=status.state; output->reason=status.reason; output->id=status.id;
			output->changed=status.changed; output->count=status.count; output->maximum=status.maximum; output->mask=status.mask;
			for(unsigned i=0;i<16;++i) output->nodes[i]=status.mask&(1U<<i)?i+1:0;
			return 0;
		}
		uint32 GetNode(GuestNode* output,uint16 id) {
			SchedulerGuard guard; if(!output || !Range(output,sizeof(*output))) return Result(UdsResult::Argument);
			if(!host || !host->Initialized()) return Result(UdsResult::NotInitialized);
			const auto* node=host->Node(id); if(!node) return LegacyResult(-3,8,1018);
			*output={}; output->friendCode=node->friend_code; output->friendNode=id; output->id=id;
			for(unsigned i=0;i<10;++i) output->name[i]=node->name[i]; return 0;
		}
		uint32 Poll(uint8 flags) {
			SchedulerGuard guard;
			while(host && host->Initialized()) {
				Pump(coreinit::OSGetTime(),nullptr);
				if(host->HasStateChange()) return 0;
				if(flags&1) return Result(UdsResult::NotFound);
				coreinit::OSWaitEventInternal(events.GetPtr());
			}
			return Result(UdsResult::NotInitialized);
		}
		uint32 ReceiveFrom(const uint32be* fd,void* buffer,uint32be* receivedSize,uint16be* source,uint32 size,uint8 flags) {
			SchedulerGuard guard;
			if(!fd || !receivedSize || !Range(fd,4) || !Range(receivedSize,4) || !Range(buffer,size) || (source && !Range(source,2)) || (flags&~1U)) {
				return Result(UdsResult::Argument);
			}
			*receivedSize=0; if(source) *source=0;
			while(host && host->Initialized()) {
				Pump(coreinit::OSGetTime(),nullptr); UdsDatagram packet;
				const auto result=host->Receive(*fd,size,packet);
				if(result==UdsResult::Ok) {
					std::memcpy(buffer,packet.data.data(),packet.data.size()); *receivedSize=static_cast<uint32>(packet.data.size()); if(source) *source=packet.source; return 0;
				}
				// NO_WAIT is a successful zero-byte receive when no packet is queued.
				if(result==UdsResult::NotFound && (flags&1)) return 0;
				if(result!=UdsResult::NotFound) return Result(result);
				const auto index=host->EndpointIndex(*fd); if(index<0) return Result(UdsResult::Argument);
				coreinit::OSWaitEventInternal(events.GetPtr()+index+1);
			}
			return Result(UdsResult::NotInitialized);
		}
		uint32 Receive(const uint32be* fd,void* buffer,uint32be* receivedSize,uint32 size,uint8 flags) {return ReceiveFrom(fd,buffer,receivedSize,nullptr,size,flags);}
		uint32 SendTo(const uint32be* fd,const void* buffer,uint32 size,uint16 node,uint8 channel,uint8 flags) {
			SchedulerGuard guard;
			if(!fd || !Range(fd,4) || !Range(buffer,size) || (flags&~3U)) {
				return Result(UdsResult::Argument);
			}
			if(!host) return Result(UdsResult::NotInitialized);
			const auto result=host->Send(*fd,node,channel,{static_cast<const uint8*>(buffer),size});
			Pump(coreinit::OSGetTime(),nullptr); return Result(result);
		}
	}

	void OnTitleStop() {Finalize();}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "nn_uds";
		}

		void RPLMapped() override
		{
			osLib_addFunction("nn_uds", "__sti___11_uds_Api_cpp_f5d9abb2", nnUdsExport___sti___11_uds_Api_cpp_f5d9abb2);
			cafeExportRegisterFunc(Initialize,"nn_uds","Initialize__Q3_2nn3uds4CafeFPvUi",LogType::Placeholder);
			cafeExportRegisterFunc(InitializeUser,"nn_uds","Initialize__Q3_2nn3uds4CafeFPvUiPQ4_2nn3cfg3CTR8UserName",LogType::Placeholder);
			cafeExportRegisterFunc(Finalize,"nn_uds","Finalize__Q3_2nn3uds4CafeFv",LogType::Placeholder);
			cafeExportRegisterFunc(CreateCommunicationId,"nn_uds","CreateLocalCommunicationId__Q3_2nn3uds4CafeFUib",LogType::Placeholder);
			cafeExportRegisterFunc(CreateNetwork,"nn_uds","CreateNetwork__Q3_2nn3uds4CafeFUcT1UiPCcT3T1PCvT3",LogType::Placeholder);
			cafeExportRegisterFunc(CreateNetworkSimple,"nn_uds","CreateNetwork__Q3_2nn3uds4CafeFUcT1UiPCcT3T1",LogType::Placeholder);
			cafeExportRegisterFunc(CreateNetworkDefault,"nn_uds","CreateNetwork__Q3_2nn3uds4CafeFUcT1UiPCcT3",LogType::Placeholder);
			cafeExportRegisterFunc(Allow,"nn_uds","AllowToConnect__Q3_2nn3uds4CafeFv",LogType::Placeholder);
			cafeExportRegisterFunc(Disallow,"nn_uds","DisallowToConnect__Q3_2nn3uds4CafeFb",LogType::Placeholder);
			cafeExportRegisterFunc(EjectClient,"nn_uds","EjectClient__Q3_2nn3uds4CafeFUs",LogType::Placeholder);
			cafeExportRegisterFunc(SetApplicationData,"nn_uds","SetApplicationDataToBeacon__Q3_2nn3uds4CafeFPCvUi",LogType::Placeholder);
			cafeExportRegisterFunc(DestroyNetwork,"nn_uds","DestroyNetwork__Q3_2nn3uds4CafeFv",LogType::Placeholder);
			cafeExportRegisterFunc(CreateEndpoint,"nn_uds","CreateEndpoint__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe18EndpointDescriptor",LogType::Placeholder);
			cafeExportRegisterFunc(Attach,"nn_uds","Attach__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe18EndpointDescriptorUsUcUi",LogType::Placeholder);
			cafeExportRegisterFunc(DestroyEndpoint,"nn_uds","DestroyEndpoint__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe18EndpointDescriptor",LogType::Placeholder);
			cafeExportRegisterFunc(GetStatus,"nn_uds","GetConnectionStatus__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe16ConnectionStatus",LogType::Placeholder);
			cafeExportRegisterFunc(GetNode,"nn_uds","GetNodeInformation__Q3_2nn3uds4CafeFPQ4_2nn3uds4Cafe15NodeInformationUs",LogType::Placeholder);
			cafeExportRegisterFunc(Poll,"nn_uds","PollStateChange__Q3_2nn3uds4CafeFUc",LogType::Placeholder);
			cafeExportRegisterFunc(ReceiveFrom,"nn_uds","ReceiveFrom__Q3_2nn3uds4CafeFRCQ4_2nn3uds4Cafe18EndpointDescriptorPvPUiPUsUiUc",LogType::Placeholder);
			cafeExportRegisterFunc(Receive,"nn_uds","Receive__Q3_2nn3uds4CafeFRCQ4_2nn3uds4Cafe18EndpointDescriptorPvPUiUiUc",LogType::Placeholder);
			cafeExportRegisterFunc(SendTo,"nn_uds","SendTo__Q3_2nn3uds4CafeFRCQ4_2nn3uds4Cafe18EndpointDescriptorPCvUiUsUcT5",LogType::Placeholder);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				udsWorkspace = nullptr;
				Finalize();
				host.reset(); alarm=nullptr;
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				udsWorkspace = nullptr;
				Finalize();
			}
		}
	}s_COSnnUdsModule;

	COSModule* GetModule()
	{
		return &s_COSnnUdsModule;
	}
}
