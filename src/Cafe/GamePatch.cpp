#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OrochiHitLight.h"
#include "Cafe/WiiFitCursorBounds.h"
#include "Cafe/ZombiIntroDecode.h"
#include "Cafe/WatchDogsVisibility.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "CafeSystem.h"
#include "config/ActiveSettings.h" // Selectively add some patches based on network settings.
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HobbitDirectMapLight.h"
#include "Cafe/NbaMoviePublish.h"
#include "Cafe/NbaPackedMovie.h"
#include "Cafe/NbaHomeAudio.h"
#include "Cafe/NbaNativeRowDispatch.h"
#include "Cafe/NbaNativeMovieUpdate.h"
#include "Cafe/NbaMovieResumeTime.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/RiseMovieFrame.h"
#include "Cafe/HW/Latte/Core/WindWakerDepthPeek.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include <mutex>
#include <unordered_map>

namespace
{
	void NbaDispatchNativeMovieUpdate(PPCInterpreter_t* cpu)
	{
		const auto movie = cpu->gpr[3];
		const bool valid = memory_isAddressRangeAccessible(movie, 0x9c00) &&
			memory_readU32(movie + 0x183c) == 1024 && memory_readU32(movie + 0x1840) == 576 &&
			memory_readU32(movie + 0x9be4) != 0;
		const auto sample = NbaMovieResumeTime::recovery.FirstUpdate(movie, cpu->gpr[4], coreinit::OSGetTime(), valid);
		if (sample.resumed)
			cemuLog_log(LogType::Force, "[COMPAT:NBA-HOME:RESUME-TIME] input_ms={} suspended_ms={} output_ms={} corrected={} native_index={} native_elapsed_ms={}",
				cpu->gpr[4], sample.suspendedMs, sample.delta, sample.corrected,
				valid ? memory_readU32(movie + 0x9be4) : 0, valid ? memory_readU32(movie + 0x9bec) : 0);
		osLib_returnFromFunction(cpu, PPCCoreCallback(NbaNativeMovieUpdate::Native,
			movie, sample.delta, cpu->gpr[5], cpu->gpr[6]));
	}
	void NbaDispatchNativeMovieRow(PPCInterpreter_t* cpu)
	{
		// Preserve the native callback path exercised by the accepted run; no host reconstruction.
		const bool trace = NbaHomeAudio::TraceMovie();
		const auto generation = trace ? NbaHomeMovieTiming::counters.Generation() : 0;
		const auto start = trace ? NbaHomeMovieTiming::NowNs() : 0;
		const auto result = PPCCoreCallback(NbaNativeRowDispatch::Native, cpu->gpr[3], cpu->gpr[4]);
		if (trace) NbaHomeMovieTiming::counters.RecordRow(generation, NbaHomeMovieTiming::NowNs() - start);
		osLib_returnFromFunction(cpu, result);
	}
	void PatchNbaNativeMovieRows()
	{
		uint32 hash=2166136261u;
		for (uint32 offset=0;offset<NbaNativeRowDispatch::NativeSize;++offset)
			hash=(hash^memory_readU8(NbaNativeRowDispatch::Native+offset))*16777619u;
		if (hash!=NbaNativeRowDispatch::NativeHash || memory_readU32(0x1018188c)!=0x3f800000) return;
		for (auto call : NbaNativeRowDispatch::Calls)
			if (memory_readU32(call)!=(0x48000001|((NbaNativeRowDispatch::Native-call)&0x03fffffc))) return;
		const uint32 hook=0x48000000|PPC_OPC_LK|PPC_OPC_AA|PPCInterpreter_makeCallableExportDepr(NbaDispatchNativeMovieRow);
		for (auto call : NbaNativeRowDispatch::Calls) memory_writeU32(call,hook);
		cemuLog_log(LogType::Force,"[COMPAT:NBA-Row] Native movie-row dispatch enabled; host reconstruction not used");
	}
	void NbaPackMovieOutput(PPCInterpreter_t* cpu)
	{
		NbaHomeMovieTiming::Sample timing(NbaHomeAudio::TraceMovie(), static_cast<sint32>(cpu->gpr[5]), cpu->spr.UGQR[2]);
		const uint32 state = cpu->gpr[3], output = cpu->gpr[4];
		const sint32 outputPitch = static_cast<sint32>(cpu->gpr[5]);
		uint32 y = 0, u = 0, v = 0, sourcePitch = 0;
		bool eligible = memory_isAddressRangeAccessible(state, 0x24);
		if (eligible)
		{
			sourcePitch = memory_readU32(state + 12);
			eligible = NbaPackedMovie::IsLayout(memory_readU32(state + 4),
				memory_readU32(state + 8), sourcePitch, outputPitch, cpu->spr.UGQR[2]);
			y = memory_readU32(state + 0x18);
			u = memory_readU32(state + 0x1c);
			v = memory_readU32(state + 0x20);
		}
		constexpr uint32 distance = (NbaPackedMovie::Height - 1) * NbaPackedMovie::RowBytes;
		eligible = eligible && output >= distance;
		const NbaPackedMovie::Range destination{output - (eligible ? distance : 0), NbaPackedMovie::Bytes};
		const uint32 checkedPitch = eligible ? sourcePitch : 0;
		const NbaPackedMovie::Range planes[] = {
			{y, checkedPitch * (NbaPackedMovie::Height - 1) + NbaPackedMovie::Width},
			{u, checkedPitch * (NbaPackedMovie::Height / 2 - 1) + NbaPackedMovie::Width / 2},
			{v, checkedPitch * (NbaPackedMovie::Height / 2 - 1) + NbaPackedMovie::Width / 2}};
		if (eligible)
		{
			eligible = NbaPackedMovie::Valid(destination) &&
				memory_isAddressRangeAccessible(destination.address, destination.bytes);
			for (const auto& plane : planes)
				eligible = eligible && NbaPackedMovie::Valid(plane) &&
					!NbaPackedMovie::Overlap(plane, destination) &&
					memory_isAddressRangeAccessible(plane.address, plane.bytes);
		}
		if (!eligible)
		{
			cemuLog_logOnce(LogType::Force, "[COMPAT:NBA-Pack] Layout/quantization/range guard rejected host conversion; pitch={} outputPitch={} gqr2={:08x}; native fallback",
				sourcePitch, outputPitch, cpu->spr.UGQR[2]);
			osLib_returnFromFunction(cpu, PPCCoreCallback(NbaPackedMovie::Native, state, output, cpu->gpr[5]));
			return;
		}
		auto* py = static_cast<const uint8*>(memory_getPointerFromVirtualOffset(y));
		auto* pu = static_cast<const uint8*>(memory_getPointerFromVirtualOffset(u));
		auto* pv = static_cast<const uint8*>(memory_getPointerFromVirtualOffset(v));
		auto proof = NbaPackedMovie::proof.load();
		if (proof == NbaPackedMovie::Proof::Verified)
		{
			timing.SetPath(NbaHomeMovieTiming::Path::Fast);
			NbaPackedMovie::Pack(py, pu, pv, sourcePitch, static_cast<uint8*>(memory_getPointerFromVirtualOffset(output)), outputPitch);
			LatteBufferCache_notifyDCFlush(destination.address, destination.bytes);
			osLib_returnFromFunction(cpu, state);
			return;
		}
		if (proof == NbaPackedMovie::Proof::Unchecked &&
			NbaPackedMovie::proof.compare_exchange_strong(proof, NbaPackedMovie::Proof::Checking))
		{
			timing.SetPath(NbaHomeMovieTiming::Path::Parity);
			const auto nativeStart = HighResolutionTimer::now().getTick();
			const uint32 result = PPCCoreCallback(NbaPackedMovie::Native, state, output, cpu->gpr[5]);
			const auto nativeEnd = HighResolutionTimer::now().getTick();
			std::vector<uint8> expected(NbaPackedMovie::Bytes);
			NbaPackedMovie::Pack(py, pu, pv, sourcePitch, expected.data() + distance, outputPitch);
			const auto hostEnd = HighResolutionTimer::now().getTick();
			const bool matches = std::memcmp(expected.data(), memory_getPointerFromVirtualOffset(destination.address), expected.size()) == 0;
			NbaPackedMovie::proof.store(matches ? NbaPackedMovie::Proof::Verified : NbaPackedMovie::Proof::Rejected);
			cemuLog_log(LogType::Force, "[COMPAT:NBA-Pack] Native/host parity={} bytes={} native_ms={:.3f} host_ms={:.3f}; fast path {}",
				matches, expected.size(), HighResolutionTimer::getTimeDiff(nativeStart, nativeEnd) * 1000.0,
				HighResolutionTimer::getTimeDiff(nativeEnd, hostEnd) * 1000.0, matches ? "enabled" : "REJECTED");
			osLib_returnFromFunction(cpu, result);
			return;
		}
		osLib_returnFromFunction(cpu, PPCCoreCallback(NbaPackedMovie::Native, state, output, cpu->gpr[5]));
	}

	void NbaSelectMovieConversion(PPCInterpreter_t* cpu)
	{
		const uint32 movie = cpu->gpr[29];
		const bool fullFrame = memory_isAddressRangeAccessible(movie, NbaMoviePublish::Parallel + 4) &&
			NbaMoviePublish::UseFullFrame(memory_readU32(movie + NbaMoviePublish::Width),
				memory_readU32(movie + NbaMoviePublish::Height),
				memory_readU32(movie + NbaMoviePublish::OutputMode),
				memory_readU32(movie + NbaMoviePublish::Parallel));
		// Retain parallel decompression; only assemble the final packed image in one pass.
		cpu->instructionPointer = (fullFrame || cpu->gpr[6] == 0) ?
			NbaMoviePublish::FullConversion : NbaMoviePublish::ParallelConversion;
		if (fullFrame)
		{
			NbaMovieResumeTime::recovery.NoteMovie(movie);
			if (NbaHomeCheck::Enabled()) NbaHomeCheck::movieFrames.fetch_add(1, std::memory_order_relaxed);
			if (NbaHomeAudio::TraceMovie() && memory_isAddressRangeAccessible(movie, 0x9be8))
				NbaHomeMovieTiming::counters.RecordMovie(memory_readU32(movie + 0x9be4));
			cemuLog_logOnce(LogType::Force, "[COMPAT:NBA-Publish] Full-frame output conversion selected; parallel decode retained");
		}
	}

	void PatchNbaMoviePublish(RPLModule* module)
	{
		if (!NbaMoviePublish::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(),
			module->moduleName2, module->patchCRC))
			return;
		for (const auto& instruction : NbaMoviePublish::Instructions)
			if (memory_readU32(instruction[0]) != instruction[1])
				return;
		memory_writeU32(NbaMoviePublish::SelectConversion, 0x48000000 | PPC_OPC_AA |
			PPCInterpreter_makeCallableExportDepr(NbaSelectMovieConversion));
		NbaPackedMovie::proof.store(NbaPackedMovie::Proof::Unchecked);
		memory_writeU32(NbaPackedMovie::Call, 0x48000000 | PPC_OPC_LK | PPC_OPC_AA |
			PPCInterpreter_makeCallableExportDepr(NbaPackMovieOutput));
		PatchNbaNativeMovieRows();
		if (memory_readU32(NbaNativeMovieUpdate::Call) == 0x4802cd49 &&
			memory_readU32(NbaNativeMovieUpdate::Call - 4) == 0x7f24cb78 &&
			memory_readU32(NbaNativeMovieUpdate::Native) == 0x9421ffc8 &&
			memory_readU32(0x02f98a1c) == 0x3bfff2cc)
		{
			NbaMovieResumeTime::recovery.Reset();
			memory_writeU32(NbaNativeMovieUpdate::Call, 0x48000000 | PPC_OPC_LK | PPC_OPC_AA |
				PPCInterpreter_makeCallableExportDepr(NbaDispatchNativeMovieUpdate));
			cemuLog_log(LogType::Force, "[COMPAT:NBA-HOME:UPDATE] Exact-build native updater callback boundary enabled; original arguments and timing retained");
		}
		cemuLog_log(LogType::Force, "[COMPAT:NBA-Publish] Scoped completed-frame handoff enabled");
	}

	void ZombiDecodeIntro(PPCInterpreter_t* cpu)
	{
		const uint32 movie = cpu->gpr[3];
		const bool intro = memory_isAddressRangeAccessible(movie, 0x4e0) && ZombiIntroDecode::IsIntro(
			memory_readU32(movie), memory_readU32(movie + 4), memory_readU32(movie + 8),
			memory_readU32(movie + 20), memory_readU32(movie + 24));
		const uint32 result = PPCCoreCallback(ZombiIntroDecode::Async, movie, cpu->gpr[4], cpu->gpr[5]);
		if (intro && result == 1)
		{
			// Retain native async decoding and buffer rotation, but finish the job
			// before the caller can publish or rebind its alternating movie banks.
			PPCCoreCallback(ZombiIntroDecode::Wait, movie, 0xffffffffu);
			cemuLog_logOnce(LogType::Force, "ZombiU intro: decoder completion serialized before publication");
		}
		osLib_returnFromFunction(cpu, result);
	}

	void PatchZombiIntroDecode()
	{
		if (!ZombiIntroDecode::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) ||
			memory_readU32(ZombiIntroDecode::Wait) != 0x9421ffd0 ||
			memory_readU32(ZombiIntroDecode::Async) != 0x7c0802a6)
			return;
		for (const auto& call : ZombiIntroDecode::Calls)
			if (memory_readU32(call[0]) != call[1])
				return;
		const uint32 replacement = PPCInterpreter_makeCallableExportDepr(ZombiDecodeIntro);
		ZombiIntroDecode::BeginStartup();
		for (const auto& call : ZombiIntroDecode::Calls)
			memory_writeU32(call[0], 0x48000000 | PPC_OPC_LK | PPC_OPC_AA | replacement);
		cemuLog_log(LogType::Force, "ZombiU: scoped intro decoder candidate installed");
	}

	void PatchWiiFitCursorBounds()
	{
		if (!WiiFitCursorBounds::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()))
			return;
		for (auto address : WiiFitCursorBounds::Reads)
			if (memory_readU32(address) != WiiFitCursorBounds::Expected)
				return;
		// Verify the two-device initializer as well as all affected reads.
		if (memory_readU32(0x025374e4) != 0x97dfe0f0 ||
			memory_readU32(0x025374f0) != 0x901f0004)
			return;
		for (auto address : WiiFitCursorBounds::Reads)
			memory_writeU32(address, WiiFitCursorBounds::AbsentDevice);
		cemuLog_log(LogType::Force, "Wii Fit U: bounded UI cursor lookup to initialized pointing devices");
	}
	void RiseRegisterMovieBuffers(PPCInterpreter_t* cpu)
	{
		const uint32 bink = cpu->gpr[3], buffers = cpu->gpr[4];
		const bool valid = memory_isAddressRangeAccessible(bink, 0x530) &&
			memory_isAddressRangeAccessible(buffers, 0x78);
		const bool refresh = valid && RiseMovieFrame::NeedsRefresh(
			memory_readU32(bink + 12), memory_readU32(bink + 0x50c),
			memory_readU32(bink + 0xe0), buffers, memory_readU32(buffers),
			memory_readU32(buffers + 4), memory_readU32(buffers + 8));
		const uint32 result = PPCCoreCallback(0x0430427c, bink, buffers);
		if (refresh)
		{
			// Preload decoded frame one into different buffers. Re-decode the keyframe
			// instead of retaining a completed-frame marker for uninitialized storage.
			memory_writeU32(bink + 0x50c, 0);
			cemuLog_logOnce(LogType::Force, "Transformers Rise: refreshing first movie frame after buffer replacement");
		}
		osLib_returnFromFunction(cpu, result);
	}
	void PatchRiseMovieBuffers()
	{
		if (!RiseMovieFrame::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) ||
			memory_readU32(0x023927a0) != 0x49f71add ||
			memory_readU32(0x0430427c) != 0x2c030000 ||
			memory_readU32(0x04306a34) != 0x8163050c)
			return;
		memory_writeU32(0x023927a0, 0x48000000 | PPC_OPC_LK | PPC_OPC_AA |
			PPCInterpreter_makeCallableExportDepr(RiseRegisterMovieBuffers));
	}
	void WindWakerReadDepth(PPCInterpreter_t* cpu)
	{
		const uint32 object = cpu->gpr[3];
		if (!memory_isAddressRangeAccessible(object, 4 + 64 * 8))
		{
			osLib_returnFromFunction(cpu, PPCCoreCallback(0x0252e388, object));
			return;
		}
		const uint32 count = memory_readU8(object);
		if (!count)
		{
			osLib_returnFromFunction(cpu, 0);
			return;
		}
		const uint32 manager = memory_readU32(0x101f95d0);
		uint32 surface = 0;
		if (memory_isAddressRangeAccessible(manager, 0x1028))
		{
			const uint32 table = memory_readU32(manager + 0x1024);
			const uint32 entry = table + (memory_readU32(manager + 0x1020) > 1 ? 4 : 0);
			if (memory_isAddressRangeAccessible(entry, 4))
				surface = memory_readU32(entry) + 0x6b3c;
		}
		if (count <= 64 && memory_isAddressRangeAccessible(surface, 0x80) &&
			memory_readU32(surface) == 0x3a && memory_readU32(surface + 0x18) == 0x80e)
		{
			std::array<WindWakerDepthPeek::Point, 64> points{};
			std::array<uint32, 64> destinations{};
			bool valid = true;
			for (uint32 i = 0; i < count; ++i)
			{
				const uint32 cell = object + 4 + i * 8;
				points[i].x = static_cast<sint16>(memory_readU16(cell));
				points[i].y = static_cast<sint16>(memory_readU16(cell + 2));
				destinations[i] = memory_readU32(cell + 4);
				valid &= memory_isAddressRangeAccessible(destinations[i], 4);
			}
			// The guest reads the main depth surface directly from CPU RAM. Submit
			// preceding draws, then answer its existing 640x480 peeks with GPU depth.
			if (valid && GX2::GX2DrawDone() && WindWakerDepthPeek::ReadPixels(
				memory_virtualToPhysical(memory_readU32(surface + 0x28)), memory_readU32(surface + 8),
				memory_readU32(surface + 12), std::span(points.data(), count)))
			{
				for (uint32 i = 0; i < count; ++i)
					memory_writeU32(destinations[i], points[i].depth);
				memory_writeU8(object, 0);
				osLib_returnFromFunction(cpu, 0);
				return;
			}
		}
		osLib_returnFromFunction(cpu, PPCCoreCallback(0x0252e388, object));
	}

	void PatchWindWakerDepthPeek()
	{
		if (!WindWakerDepthPeek::IsTarget(CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) ||
			memory_readU32(0x025f0404) != 0x4bf3df85 || memory_readU32(0x0252e388) != 0x9421fea0)
			return;
		const uint32 function = PPCInterpreter_makeCallableExportDepr(WindWakerReadDepth);
		memory_writeU32(0x025f0404, 0x48000000 | PPC_OPC_LK | PPC_OPC_AA | function);
		cemuLog_log(LogType::Force, "Wind Waker HD: GPU depth peeks enabled");
	}
}

namespace
{
	constexpr uint32 kBayonetta2BankCall = 0x012ee26c;
	constexpr uint32 kBayonetta2BankWrapper = 0x012eb228;
	constexpr uint32 kBayonetta2NativeLoadBank = 0x0152a8f4;

	void Bayonetta2LoadNarrationBank(PPCInterpreter_t* hCPU)
	{
		const uint32 text = hCPU->spr.LR - (kBayonetta2BankCall + 4);
		const uint32 bank = hCPU->gpr[4];
		const uint32 size = hCPU->gpr[5];
		const uint32 object = hCPU->gpr[3];
		const uint64 end = static_cast<uint64>(bank) + size;
		if (size == 219342 && bank >= 0x10000000 && end <= 0x50000000 && (bank & 63) == 0 &&
			(bank >> 29) != (end >> 29) && memory_readU32(object + 4) == 0 &&
			memory_readU32(bank) == 0x424b4844 && memory_readU32(bank + 4) == 24 &&
			memory_readU32(bank + 8) == 88 && memory_readU32(bank + 12) == 0x5570f90c)
		{
			// Wwise software-decodes vo_r6a0 into separate PCM output rings. The
			// wrapper's hardware-voice boundary check can reject this intact bank
			// under Cemu's heap layout. Retain native bank validation and ownership.
			const uint32 result = PPCCoreCallback(text + kBayonetta2NativeLoadBank, bank, size, object + 4) == 1;
			if (result)
			{
				memory_writeU32(object + 8, bank);
				cemuLog_log(LogType::Force, "[COMPAT:Bayonetta2] Registered narration bank across hardware-voice boundary at 0x{:08x}", bank);
			}
			osLib_returnFromFunction(hCPU, result);
			return;
		}
		osLib_returnFromFunction(hCPU, PPCCoreCallback(text + kBayonetta2BankWrapper, object, bank, size, hCPU->gpr[6]));
	}

	void PatchBayonetta2NarrationBank(RPLModule* module)
	{
		if (CafeSystem::GetForegroundTitleId() != 0x0005000010172600ULL ||
			CafeSystem::GetForegroundTitleVersion() != 0 || CafeSystem::GetRPXHashUpdated() != 0xb8611392 ||
			module->moduleName2 != "prj_010" || module->patchCRC != 0xaf5d1a85 ||
			module->sectionAddressTable2.size() <= 2 || !module->sectionAddressTable2[2].ptr)
			return;
		const uint32 text = memory_getVirtualOffsetFromPointer(module->sectionAddressTable2[2].ptr);
		const std::pair<uint32, uint32> expected[] = {
			{kBayonetta2BankCall - 4, 0x7f84e378},
			{kBayonetta2BankCall, 0x4bffcfbd},
			{kBayonetta2BankCall + 4, 0x2c030000},
			{kBayonetta2BankWrapper, 0x7c0802a6},
			{kBayonetta2BankWrapper + 0x28, 0x57e006bf},
			{kBayonetta2BankWrapper + 0x48, 0x40800054},
			{kBayonetta2BankWrapper + 0x90, 0x4823f63d},
			{kBayonetta2BankWrapper + 0xb8, 0x93fe0008},
			{kBayonetta2NativeLoadBank, 0x7c0802a6},
			{kBayonetta2NativeLoadBank + 4, 0x9421ff78},
		};
		for (const auto& [offset, opcode] : expected)
			if (memory_readU32(text + offset) != opcode)
				return;
		const uint32 function = PPCInterpreter_makeCallableExportDepr(Bayonetta2LoadNarrationBank);
		memory_writeU32(text + kBayonetta2BankCall, 0x48000000 | PPC_OPC_LK | PPC_OPC_AA | function);
		cemuLog_log(LogType::Force, "[COMPAT:Bayonetta2] Narration bank compatibility safeguard enabled");
	}
}

namespace
{
	constexpr uint32 kBayonettaMovieStartCall = 0x00dcc418;
	constexpr uint32 kBayonettaAudioStart = 0x00d4ddec;
	constexpr uint32 kBayonettaAudioMode = 0x00d2c08c;
	constexpr uint32 kBayonettaAudioLock = 0x00d2ba44;
	constexpr uint32 kBayonettaAudioUnlock = 0x00d2ba8c;
	constexpr uint32 kBayonettaSetCallback = 0x00d2c2bc;
	constexpr uint32 kBayonettaMovieCallback = 0x00dcbd80;

	void BayonettaStartMovieAudio(PPCInterpreter_t* hCPU)
	{
		const uint32 text = hCPU->spr.LR - (kBayonettaMovieStartCall + 4);
		const uint32 voice = hCPU->gpr[3];
		const uint32 modeAddress = (memory_readU32(text + kBayonettaAudioMode) << 16)
			+ static_cast<sint16>(memory_readU32(text + kBayonettaAudioMode + 4));
		if (memory_readU32(modeAddress) == 1)
		{
			// Mana may initialize before Atom and skip its refill callback. Retry at
			// movie start, under Atom's own lock, without replacing an existing callback.
			PPCCoreCallback(text + kBayonettaAudioLock);
			if (memory_readU32(modeAddress) == 1 &&
				static_cast<sint32>(memory_readU32(modeAddress + 0x1128)) > 0 &&
				memory_readU32(modeAddress + 0x20) == 0)
			{
				PPCCoreCallback(text + kBayonettaSetCallback, text + kBayonettaMovieCallback, uint32{0});
				cemuLog_log(LogType::Force, "[COMPAT:Bayonetta] Restored movie audio refill callback missed during initialization");
			}
			PPCCoreCallback(text + kBayonettaAudioUnlock);
		}
		osLib_returnFromFunction(hCPU, PPCCoreCallback(text + kBayonettaAudioStart, voice));
	}
}

namespace
{
	void InjusticePollMovieFrame(PPCInterpreter_t* hCPU)
	{
		const uint32 movie = hCPU->gpr[3];
		const uint32 manager = memory_readU32(0x10253500);
		if (manager >= 0x10000000 && manager < 0x40000000 - 0x40 &&
			memory_readU32(manager) == 0x10104ef8 &&
			memory_readU8(manager + 0x38) == 0 && memory_readU8(manager + 0x39) == 1)
		{
			// Apply the game's deferred enable under its own lock before the
			// renderer waits for output from that disabled decoding callback.
			PPCCoreCallback(0x028cdb58, manager);
			cemuLog_logOnce(LogType::Force, "[COMPAT:Injustice] Applied pending movie callback enable before frame wait");
		}
		osLib_returnFromFunction(hCPU, PPCCoreCallback(0x028d22e4, movie));
	}

	void PatchInjusticeMovieWait(RPLModule* module)
	{
		if (CafeSystem::GetForegroundTitleId() != 0x0005000010111700ULL ||
			CafeSystem::GetForegroundTitleVersion() != 16 || CafeSystem::GetRPXHashUpdated() != 0x3156c533 ||
			module->moduleName2 != "dcfgame-retail" || module->patchCRC != 0x8bef46ff)
			return;
		const std::pair<uint32, uint32> expected[] = {
			{0x028d2768, 0x7f63db78}, {0x028d276c, 0x3b000008}, {0x028d2770, 0x4bfffb75},
			{0x028d2774, 0x7f43d378}, {0x028d22e4, 0x4bff9418},
			{0x028cdb58, 0x7c0802a6}, {0x028cdb5c, 0x9421fff0},
			{0x028cdb78, 0x899f0038}, {0x028cdb7c, 0x8bdf0039},
			{0x028cb708, 0x3fe01025}, {0x028cb710, 0x841f3500},
		};
		for (const auto& [address, opcode] : expected)
			if (memory_readU32(address) != opcode)
				return;
		const uint32 function = PPCInterpreter_makeCallableExportDepr(InjusticePollMovieFrame);
		memory_writeU32(0x028d2770, 0x48000000 | PPC_OPC_LK | PPC_OPC_AA | function);
		cemuLog_log(LogType::Force, "[COMPAT:Injustice] Deferred movie callback safeguard enabled");
	}
}

namespace
{
	bool IsHobbitMapLightTrack(uint32 track)
	{
		if (!memory_isAddressRangeAccessible(track, 0x50) || memory_readU32(track + 0x10) != 135)
			return false;
		const uint32 property = memory_readU32(track + 0x3c);
		if (!memory_isAddressRangeAccessible(property, 11) || memory_readU8(property) != 11 ||
			std::memcmp(memory_getPointerFromVirtualOffset(property + 1), "Intensity", 10) != 0)
			return false;
		const uint32 owner = memory_readU32(track + 0x1c);
		const uint32 first = memory_readU32(track + 8);
		const uint32 last = memory_readU32(track + 0xc);
		if (!memory_isAddressRangeAccessible(owner, 0x2c) ||
			!memory_isAddressRangeAccessible(first, 0x18) || !memory_isAddressRangeAccessible(last, 0x18))
			return false;
		const uint32 name = memory_readU32(owner + 0x24);
		constexpr std::string_view expected = "DynamicLightDesc.LightDescription70Anim";
		if (!memory_isAddressRangeAccessible(name, expected.size() + 1) || memory_readU8(name + expected.size()) != 0)
			return false;
		return HobbitDirectMapLight::MatchesTrack(memory_readU32(track + 0x10),
			std::string_view(reinterpret_cast<const char*>(memory_getPointerFromVirtualOffset(name)), expected.size()),
			memory_readU16(first + 0xc), memory_readU32(first + 0x14) >> 19,
			memory_readU16(last + 0xc), memory_readU32(last + 0x14) >> 19);
	}

	std::mutex s_hobbitMapMutex;
	std::unordered_map<uint64, double> s_hobbitMapTimes;
	HobbitDirectMapLight::Filter s_hobbitMapFilter;

	void HobbitMapLightInput(PPCInterpreter_t* cpu)
	{
		if (IsHobbitMapLightTrack(cpu->gpr[29]))
		{
			std::lock_guard lock(s_hobbitMapMutex);
			if (s_hobbitMapTimes.size() < 8)
				s_hobbitMapTimes[(uint64(cpu->gpr[1]) << 32) | cpu->gpr[29]] = cpu->fpr[1].fpr;
		}
		// Preserve the original bl, including its link, without an extra guest call frame.
		cpu->spr.LR = 0x026bd718;
		cpu->instructionPointer = 0x026bd364;
	}

	void HobbitMapLightOutput(PPCInterpreter_t* cpu)
	{
		const uint32 sp = cpu->gpr[1];
		{
			std::lock_guard lock(s_hobbitMapMutex);
			const auto key = (uint64(sp) << 32) | cpu->gpr[29];
			if (const auto it = s_hobbitMapTimes.find(key); it != s_hobbitMapTimes.end())
			{
				if (IsHobbitMapLightTrack(cpu->gpr[29]) && memory_isAddressRangeAccessible(sp, 0x24) &&
					memory_readU32(sp + 0x20) == 0)
					cpu->fpr[31].fpr = s_hobbitMapFilter.Apply(cpu->gpr[29], it->second, static_cast<float>(cpu->fpr[31].fpr));
				s_hobbitMapTimes.erase(it);
			}
		}
		// The replaced lwz restores r29; the game's epilogue returns f31 through f1.
		cpu->gpr[29] = memory_readU32(sp + 0x74);
		cpu->instructionPointer = 0x026bd8a8;
	}

	void PatchHobbitMapLightCompatibility(RPLModule* module)
	{
		if (!HobbitDirectMapLight::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(),
			module->moduleName2, module->patchCRC))
			return;
		const std::pair<uint32, uint32> expected[] = {
			{0x026bd714, 0x4bfffc51}, {0x026bd718, 0x81610020},
			{0x026bd8a4, 0x83a10074}, {0x026bd8a8, 0x83e1007c},
			{0x026bd660, 0xec020828}, {0x026bd664, 0xec2008fa}
		};
		for (const auto& [address, opcode] : expected)
			if (memory_readU32(address) != opcode)
				return;
		{
			std::lock_guard lock(s_hobbitMapMutex);
			s_hobbitMapTimes.clear();
			s_hobbitMapFilter = {};
		}
		memory_writeU32(0x026bd714, 0x48000000 | PPC_OPC_AA |
			PPCInterpreter_makeCallableExportDepr(HobbitMapLightInput));
		memory_writeU32(0x026bd8a4, 0x48000000 | PPC_OPC_AA |
			PPCInterpreter_makeCallableExportDepr(HobbitMapLightOutput));
		cemuLog_log(LogType::Force, "[COMPAT:Hobbit] Direct map-light compatibility smoothing enabled (workaround; no graphic pack; animation retained)");
	}
}

namespace
{
	void AvengersAwaitAudioBeforeEntities(PPCInterpreter_t* cpu)
	{
		constexpr uint32 isInitialized = 0x034ce59c;
		const uint32 flag = (memory_readU32(isInitialized) << 16) +
			static_cast<sint16>(memory_readU32(isInitialized + 4));
		// The title delays audio init on core 2, but its entity loader can finish
		// first. Defer that tick instead of invoking ClockModel's RTPC on null.
		if (memory_readU8(flag) == 0)
		{
			cemuLog_logOnce(LogType::Force, "[COMPAT:Avengers] Waiting for audio initialization before entity activation");
			cpu->instructionPointer = 0x02c9fce4;
			return;
		}
		cpu->gpr[12] = memory_readU32(cpu->gpr[3] + 0x84);
		cpu->instructionPointer = 0x02c9fccc;
	}

	void PatchAvengersAudioStartup(RPLModule* module)
	{
		if (CafeSystem::GetForegroundTitleId() != 0x000500001010af00ULL ||
			CafeSystem::GetForegroundTitleVersion() != 16 || CafeSystem::GetRPXHashUpdated() != 0xb57ad7ba ||
			module->moduleName2 != "game_wiiu_rpl" || module->patchCRC != 0xdf0dea6e)
			return;
		const std::pair<uint32, uint32> expected[] = {
			{0x02c9fcc8, 0x81830084}, {0x02c9fccc, 0x880c0221},
			{0x02c9fce0, 0x4bffec31}, {0x02c9fce4, 0x38600001},
			{0x02c9fce8, 0x80010014}, {0x034ce5a4, 0x4e800020},
			{0x0292cbe8, 0x7fc3f378}, {0x0292cbec, 0x48b02fa1}
		};
		for (const auto& [address, opcode] : expected)
			if (memory_readU32(address) != opcode)
				return;
		if ((memory_readU32(0x034ce59c) & 0xffff0000) != 0x3d800000 ||
			(memory_readU32(0x034ce5a0) & 0xffff0000) != 0x886c0000)
			return;
		const uint32 flag = (memory_readU32(0x034ce59c) << 16) +
			static_cast<sint16>(memory_readU32(0x034ce5a0));
		if (!memory_isAddressRangeAccessible(flag, 1))
			return;
		memory_writeU32(0x02c9fcc8, 0x48000000 | PPC_OPC_AA |
			PPCInterpreter_makeCallableExportDepr(AvengersAwaitAudioBeforeEntities));
		cemuLog_log(LogType::Force, "[COMPAT:Avengers] Audio-before-entity startup ordering enabled");
	}
}

void GamePatch_OnModuleLoaded(RPLModule* module)
{
	PatchNbaMoviePublish(module);
	PatchAvengersAudioStartup(module);
	PatchHobbitMapLightCompatibility(module);
	PatchInjusticeMovieWait(module);
	PatchBayonetta2NarrationBank(module);
	if (CafeSystem::GetForegroundTitleId() != 0x0005000010157e00ULL ||
		CafeSystem::GetForegroundTitleVersion() != 1 || CafeSystem::GetRPXHashUpdated() != 0x52f48c53 ||
		module->moduleName2 != "bayonetwu" || module->patchCRC != 0x62350931)
		return;
	if (module->sectionAddressTable2.size() <= 1 || !module->sectionAddressTable2[1].ptr)
		return;
	const uint32 text = memory_getVirtualOffsetFromPointer(module->sectionAddressTable2[1].ptr);
	// Check the call and native helpers after relocation. Do not patch a modified RPL.
	const std::pair<uint32, uint32> expected[] = {
		{kBayonettaMovieStartCall - 4, 0x807f0144},
		{kBayonettaMovieStartCall, 0x4bf819d5},
		{kBayonettaMovieStartCall + 4, 0x807f01b4},
		{kBayonettaAudioLock, 0x48039c5c},
		{kBayonettaAudioUnlock, 0x48039c4c},
		{kBayonettaSetCallback, 0x7c0802a6},
		{kBayonettaAudioMode + 8, 0x4e800020},
		{kBayonettaMovieCallback, 0x7c0802a6},
		{kBayonettaMovieCallback + 4, 0x9421ffd0},
	};
	for (const auto& [offset, opcode] : expected)
		if (memory_readU32(text + offset) != opcode)
			return;
	if ((memory_readU32(text + kBayonettaAudioMode) & 0xffff0000) != 0x3c600000 ||
		(memory_readU32(text + kBayonettaAudioMode + 4) & 0xffff0000) != 0x80630000)
		return;
	const uint32 function = PPCInterpreter_makeCallableExportDepr(BayonettaStartMovieAudio);
	memory_writeU32(text + kBayonettaMovieStartCall, 0x48000000 | PPC_OPC_LK | PPC_OPC_AA | function);
	cemuLog_log(LogType::Force, "[COMPAT:Bayonetta] Movie audio initialization safeguard at 0x{:08x}", text + kBayonettaMovieStartCall);
}

void hleExport_breathOfTheWild_busyLoop(PPCInterpreter_t* hCPU)
{
	uint32 queue7C = memory_readU32(hCPU->gpr[24] + 0x7C);
	uint32 queue80b = memory_readU8(hCPU->gpr[24] + 0x80);

	if (!(queue80b == 0 || hCPU->gpr[22] != 0 || queue7C > 0))
	{
		PPCInterpreter_relinquishTimeslice();
	}

	hCPU->gpr[6] = hCPU->gpr[29];
	hCPU->instructionPointer += 4;
}

void hleExport_breathOfTheWild_busyLoop2(PPCInterpreter_t* hCPU)
{
	uint32 queue7C = memory_readU32(hCPU->gpr[24] + 0x7C);
	uint32 queue80b = memory_readU8(hCPU->gpr[24] + 0x80);

	if (!(queue80b == 0 || hCPU->gpr[22] != 0 || queue7C > 0))
	{
		PPCInterpreter_relinquishTimeslice();
	}

	hCPU->gpr[12] = hCPU->gpr[29];
	hCPU->instructionPointer += 4;
}

void hleExport_ffl_swapEndianFloatArray(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(valueArray, uint32, 0);
	ppcDefineParamS32(valueCount, 1);
	for (sint32 i = 0; i < valueCount; i++)
	{
		valueArray[i] = _swapEndianU32(valueArray[i]);
	}
	osLib_returnFromFunction(hCPU, 0);
}

typedef struct  
{
	std::atomic<uint32be> count;
	uint32be ownerThreadId;
	uint32 ukn08;
}xcxCS_t;

void hleExport_xcx_enterCriticalSection(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(xcxCS, xcxCS_t, 0);
	uint32 threadId = MEMPTR<OSThread_t>(coreinit::OSGetCurrentThread()).GetMPTR();
	cemu_assert_debug(xcxCS->ukn08 != 0);
	cemu_assert_debug(threadId);
	if (xcxCS->ownerThreadId == (uint32be)threadId)
	{
		xcxCS->count.store(xcxCS->count.load() + 1);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	// quick check
	uint32be newCount = xcxCS->count.load() + 1;
	uint32be expectedCount = 0;
	if(xcxCS->count.compare_exchange_strong(expectedCount, newCount))
	{
		xcxCS->ownerThreadId = threadId;
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	// spinloop for a bit to reduce the time we occupy the scheduler lock (via PPCCore_switchToScheduler)
	while (true)
	{
		for (sint32 i = 0; i < 50; i++)
		{
			if (xcxCS->count.compare_exchange_strong(expectedCount, newCount))
			{
				xcxCS->ownerThreadId = threadId;
				osLib_returnFromFunction(hCPU, 0);
				return;
			}
            _mm_pause();
		}
		PPCCore_switchToScheduler();
	}
	osLib_returnFromFunction(hCPU, 0);
}

bool mh3u_raceConditionWorkaround = true;

void hleExport_mh3u_raceConditionWorkaround(PPCInterpreter_t* hCPU) // new style HLE method, does not need entry in hle_load (but can only be reached via BL and not the usual HLE instruction)
{
	uint8 b = memory_readU8(hCPU->gpr[3] + 0x3E5);
	b ^= 1;
	if (mh3u_raceConditionWorkaround)
	{
		b = 0;
		mh3u_raceConditionWorkaround = false;
	}
	osLib_returnFromFunction(hCPU, b);
}

void hleExport_pmcs_yellowPaintStarCrashWorkaround(PPCInterpreter_t* hCPU)
{
	hCPU->gpr[7] = hCPU->gpr[3] * 4;
	MPTR parentLR = memory_readU32(hCPU->gpr[1] + 0x4C);
	if (hCPU->gpr[3] >= 0x00800000)
	{
		hCPU->instructionPointer = parentLR;
		hCPU->gpr[1] += 0x48;
		hCPU->gpr[3] = 0;
		return;
	}
	hCPU->instructionPointer = hCPU->spr.LR;
}

void hleExport_mn9_initResourceGuard(PPCInterpreter_t* hCPU)
{
	constexpr uint32 kCodeStart = MEMORY_CODEAREA_ADDR;
	constexpr uint32 kCodeEnd = MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE;
	const uint32 resource = hCPU->gpr[3];
	const uint32 target = hCPU->gpr[0];

	if (target >= kCodeStart && target < kCodeEnd)
	{
		PPCCoreCallback(target, resource);
	}
	else
	{
		static std::atomic_uint32_t invalidCallCount{ 0 };
		const uint32 count = invalidCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count <= 8 || (count & (count - 1)) == 0)
		{
			cemuLog_log(LogType::Force,
				"[COMPAT:MN9] event=skipped-invalid-init-resource-call resource=0x{:08x} vtable=0x{:08x} target=0x{:08x} count={}",
				resource, hCPU->gpr[12], target, count);
		}
	}

	// This hook replaces the original bctrl, so resume at its link address.
	hCPU->instructionPointer = hCPU->spr.LR;
}

void hleExport_mn9_updateTransformGuard(PPCInterpreter_t* hCPU)
{
	constexpr uint32 kRemoveFromScene = 0x02A95150;
	constexpr uint32 kUpdateTransformEpilogue = 0x02B1099C;
	const uint32 sceneInfo = hCPU->gpr[3];
	const bool sceneInfoAccessible = memory_isAddressRangeAccessible(sceneInfo, 8);
	const uint32 proxy = sceneInfoAccessible ? memory_readU32(sceneInfo + 4) : 0;
	const bool proxyAccessible = proxy > 1 && memory_isAddressRangeAccessible(proxy, 0x8C);

	if (sceneInfoAccessible && proxyAccessible)
	{
		const uint32 resumeAddress = hCPU->spr.LR;
		PPCCoreCallback(kRemoveFromScene, sceneInfo);
		hCPU->instructionPointer = resumeAddress;
		return;
	}

	static std::atomic_uint32_t staleCommandCount{ 0 };
	const uint32 count = staleCommandCount.fetch_add(1, std::memory_order_relaxed) + 1;
	if (count <= 8 || (count & (count - 1)) == 0)
	{
		cemuLog_log(LogType::Force,
			"[COMPAT:MN9] event=skipped-stale-transform-command sceneInfo=0x{:08x} proxy=0x{:08x} count={}",
			sceneInfo, proxy, count);
	}

	// Return through the command's original epilogue so its stack frame is unwound normally.
	hCPU->instructionPointer = kUpdateTransformEpilogue;
}

void hleExport_orochiHitLightDivide(PPCInterpreter_t* cpu)
{
	if (cpu->gpr[3] == cpu->gpr[4] && memory_isAddressRangeAccessible(cpu->gpr[4], 16))
	{
		std::array<uint32, 4> color;
		for (uint32 i = 0; i < color.size(); ++i)
			color[i] = memory_readU32(cpu->gpr[4] + i * 4);
		if (OrochiHitLight::IsFiniteZeroDivide(cpu->fpr[1].fp0, color))
		{
			// Preserve finite hit colors during the zero-divisor transition.
			// Otherwise its reciprocal refinement produces persistent NaN colors.
			cpu->instructionPointer = cpu->spr.LR;
			return;
		}
	}
	cpu->instructionPointer = 0x020c2094;
}

void WatchDogsPendingRequests(PPCInterpreter_t* cpu)
{
	if (!cpu->gpr[3])
	{
		cemuLog_logOnce(LogType::Force, "Watch Dogs: waiting for request-manager initialization");
		osLib_returnFromFunction(cpu, 1);
		return;
	}
	cpu->instructionPointer = 0x04e49e34;
}

void hleExport_mn9_appRequestExitTrace(PPCInterpreter_t* hCPU)
{
	constexpr uint32 kAppRequestExitAfterMflr = 0x02E8E00C;
	const uint32 caller = hCPU->spr.LR;
	const uint32 reason = hCPU->gpr[3];
	const uint32 thread = MEMPTR<OSThread_t>(coreinit::OSGetCurrentThread()).GetMPTR();

	cemuLog_log(LogType::Force,
		"[COMPAT:MN9] event=app-request-exit reason={} caller=0x{:08x} thread=0x{:08x} sp=0x{:08x}",
		reason, caller, thread, hCPU->gpr[1]);
	DebugLogStackTrace(coreinit::OSGetCurrentThread(), hCPU->gpr[1]);

	// Preserve the replaced mflr r0 before continuing the original function.
	hCPU->gpr[0] = caller;
	hCPU->instructionPointer = kAppRequestExitAfterMflr;
}

namespace
{
	struct MN9DeferredCleanupBatch
	{
		uint32 object{};
		uint32 deleteFlags{};
	};

	MN9DeferredCleanupBatch s_mn9DeferredCleanupBatch;
}

void hleExport_mn9_pendingCleanupGuard(PPCInterpreter_t* hCPU)
{
	constexpr uint32 kPendingCleanupDestructor = 0x02320F44;
	const uint32 pendingCleanup = hCPU->gpr[3];
	const uint32 deleteFlags = hCPU->gpr[4];
	const uint32 resumeAddress = hCPU->spr.LR;
	const MN9DeferredCleanupBatch previousBatch = s_mn9DeferredCleanupBatch;
	s_mn9DeferredCleanupBatch = {};

	if (previousBatch.object != 0)
	{
		// The callsite has just completed the next native render fence. Releasing the
		// prior batch here preserves ordering without stalling every frame on a full flush.
		cemuLog_logOnce(LogType::Force, "Mighty No. 9: retired deferred cleanup after the extra fence");
		const uint32 cleanupCount = memory_readU32(previousBatch.object + 4);
		const HRTick cleanupStart = HighResolutionTimer::now().getTick();
		PPCCoreCallback(kPendingCleanupDestructor, previousBatch.object, previousBatch.deleteFlags);
		const double cleanupMs = HighResolutionTimer::getTimeDiff(cleanupStart, HighResolutionTimer::now().getTick()) * 1000.0;
		if (cleanupMs >= 50.0)
		{
			cemuLog_log(LogType::Force,
				"[COMPAT:MN9] stall=deferred-cleanup duration_ms={:.3f} objects={} batch=0x{:08x}",
				cleanupMs, cleanupCount, previousBatch.object);
		}
	}

	if (pendingCleanup != 0)
		s_mn9DeferredCleanupBatch = { pendingCleanup, deleteFlags };

	hCPU->instructionPointer = resumeAddress;
}

uint8 hleSignature_wwhd_0173B2A0[] = {0x8D,0x43,0x00,0x01,0x7C,0xC9,0x52,0x78,0x55,0x2C,0x15,0xBA,0x7C,0x0C,0x28,0x2E,0x54,0xC8,0xC2,0x3E,0x7D,0x06,0x02,0x78,0x42,0x00,0xFF,0xE8,0x7C,0xC3,0x30,0xF8};

void hle_scan(uint8* data, sint32 dataLength, char* hleFunctionName)
{
	sint32 functionIndex = osLib_getFunctionIndex("hle", hleFunctionName);
	if( functionIndex < 0 )
	{
		debug_printf("HLE function unknown\n");
		return;
	}

	uint8* scanStart = memory_getPointerFromVirtualOffset(0x01000000);
	uint8* scanEnd = scanStart + 0x0F000000 - dataLength;
	uint8* scanCurrent = scanStart;
	while( scanCurrent < scanEnd )
	{
		if( memcmp(scanCurrent, data, dataLength) == 0 )
		{
			uint32 offset = (uint32)(scanCurrent - scanStart) + 0x01000000;
			debug_printf("HLE signature for '%s' found at 0x%08x\n", hleFunctionName, offset);
			uint32 opcode = (1<<26)|(functionIndex+0x1000); // opcode for HLE: 0x1000 + FunctionIndex
			memory_write<uint32>(offset, opcode);
			break;
		}
		scanCurrent += 4;
	}
}

MPTR hle_locate(uint8* data, sint32 dataLength)
{
	uint8* scanStart = memory_getPointerFromVirtualOffset(0x01000000);
	uint8* scanEnd = scanStart + 0x0F000000 - dataLength;
	uint8* scanCurrent = scanStart;
	while( scanCurrent < scanEnd )
	{
		if( memcmp(scanCurrent, data, dataLength) == 0 )
		{
			return memory_getVirtualOffsetFromPointer(scanCurrent);
		}
		scanCurrent += 4;
	}
	return MPTR_NULL;
}

bool compareMasked(uint8* mem, uint8* compare, uint8* mask, sint32 length)
{
	while( length )
	{
		uint8 m = *mask;
		if( (*mem&m) != (*compare&m) )
			return false;
		mem++;
		compare++;
		mask++;
		length--;
	}
	return true;
}

MPTR hle_locate(uint8* data, uint8* mask, sint32 dataLength)
{
	uint8* scanStart = memory_getPointerFromVirtualOffset(MEMORY_CODEAREA_ADDR);
	uint8* scanEnd = memory_getPointerFromVirtualOffset(RPLLoader_GetMaxCodeOffset() - dataLength);
	uint8* scanCurrent = scanStart;
	if( mask )
	{
		if (dataLength >= 4 && *(uint32*)mask == 0xFFFFFFFF)
		{
			// fast path
			uint32 firstDword = *(uint32*)data;
			while (scanCurrent < scanEnd)
			{
				if (*(uint32*)scanCurrent == firstDword && compareMasked(scanCurrent, data, mask, dataLength))
				{
					return memory_getVirtualOffsetFromPointer(scanCurrent);
				}
				scanCurrent += 4;
			}
		}
		else
		{
#ifdef CEMU_DEBUG_ASSERT
			if (mask[0] != 0xFF)
				assert_dbg();
#endif
			uint8 firstByte = data[0];
			while (scanCurrent < scanEnd)
			{
				if (scanCurrent[0] == firstByte && compareMasked(scanCurrent, data, mask, dataLength))
				{
					return memory_getVirtualOffsetFromPointer(scanCurrent);
				}
				scanCurrent += 4;
			}
		}
	}
	else
	{
		while( scanCurrent < scanEnd )
		{
			if( memcmp(scanCurrent, data, dataLength) == 0 )
			{
				return memory_getVirtualOffsetFromPointer(scanCurrent);
			}
			scanCurrent += 4;
		}
	}
	return MPTR_NULL;
}

uint8 xcx_gpuHangDetection_degradeFramebuffer[] = {0x3B,0x39,0x00,0x01,0x28,0x19,0x4E,0x20,0x40,0x81,0x00,0x44};

uint8 xcx_framebufferReductionSignature[] = {0x80,0xC9,0x00,0x1C,0x38,0xA0,0x00,0x01,0x80,0x7E,0x00,0x80,0x80,0x9E,0x02,0xEC,0x80,0xE9,0x00,0x20,0x48,0x06,0x1E,0xD1,0x7E,0x73,0x1B,0x78};
uint8 xcx_framebufferReductionMask[] =      {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF};

uint8 botw_busyLoopSignature[] = {0x80,0xE6,0x00,0x00,0x2C,0x07,0x00,0x01,0x41,0x82,0xFF,0xF8,0x7D,0x00,0x30,0x28,0x2C,0x08,0x00,0x00,0x40,0x82,0xFF,0xF8,0x7C,0x00,0x30,0x6C,0x39,0x00,0x00,0x01,0x7D,0x00,0x31,0x2D,0x40,0x82,0xFF,0xE8 };
uint8 botw_busyLoopMask[] =      {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

uint8 botw_busyLoopSignature2[] = {0x80,0x0C,0x00,0x00,0x2C,0x00,0x00,0x01,0x41,0x82,0xFF,0xF8,0x7C,0xA0,0x60,0x28,0x2C,0x05,0x00,0x00,0x40,0x82,0xFF,0xF8,0x7C,0x00,0x60,0x6C,0x38,0x80,0x00,0x01,0x7C,0x80,0x61,0x2D,0x40,0x82,0xFF,0xE8};
uint8 botw_busyLoopMask2[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

uint8 botw_crashFuncSignature[] = { 0x94,0x21,0xFF,0xD8,0x7C,0x08,0x02,0xA6,0xBF,0x41,0x00,0x10,0x7C,0xC7,0x33,0x78,0x7C,0xBE,0x2B,0x78,0x90,0x01,0x00,0x2C,0x7C,0x9D,0x23,0x78,0x38,0x00,0x00,0x00,0x7F,0xC6,0xF3,0x78,0x38,0x81,0x00,0x0C,0x90,0x01,0x00,0x0C,0x38,0xA1,0x00,0x08 };
uint8 botw_crashFuncMask[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

uint8 ffl_floatArrayEndianSwap[] = { 0x7C,0x08,0x02,0xA6,0x94,0x21,0xFF,0xE8,0x93,0xC1,0x00,0x10,0x7C,0x7E,0x1B,0x78,0x93,0xE1,0x00,0x14,0x93,0x81,0x00,0x08,0x7C,0x9F,0x23,0x78,0x93,0xA1,0x00,0x0C,0x90,0x01,0x00,0x1C,0x3B,0xA0,0x00,0x00,0x7C,0x1D,0xF8,0x40,0x40,0x80,0x00,0x20,0x57,0xBC,0x10,0x3A,0x7C,0x3E,0xE4,0x2E };

// Alternative FFL endian swap signature (Mario Kart 8 and possibly other titles)
// Same algorithm as above but with a different compiler-scheduled prologue ordering
uint8 ffl_floatArrayEndianSwap_v2[] = { 0x7C,0x08,0x02,0xA6,0x94,0x21,0xFF,0xE8,0x93,0xE1,0x00,0x14,0x93,0xA1,0x00,0x0C,0x7C,0x9F,0x23,0x78,0x93,0x81,0x00,0x08,0x93,0xC1,0x00,0x10,0x90,0x01,0x00,0x1C,0x3B,0xA0,0x00,0x00,0x7C,0x1D,0xF8,0x40,0x7C,0x7E,0x1B,0x78,0x40,0x80,0x00,0x20,0x57,0xBC,0x10,0x3A,0x7C,0x3E,0xE4,0x2E };

uint8 xcx_enterCriticalSectionSignature[] = { 0x94,0x21,0xFF,0xE0,0xBF,0x41,0x00,0x08,0x7C,0x08,0x02,0xA6,0x90,0x01,0x00,0x24,0x7C,0x7E,0x1B,0x78,0x80,0x1E,0x00,0x08,0x2C,0x00,0x00,0x00,0x41,0x82,0x00,0xC0,0x48,0x01,0xD7,0xA1,0x7C,0x7A,0x1B,0x79,0x41,0x82,0x00,0xB4,0x81,0x3E,0x00,0x04,0x7C,0x09,0xD0,0x40,0x40,0x82,0x00,0x2C,0x7D,0x20,0xF0,0x28,0x7C,0x00,0xF0,0x6C };
uint8 xcx_enterCriticalSectionMask[] =      { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

uint8 smash4_softlockFixV0Signature[] = { 0x2C,0x03,0x00,0x00,0x41,0x82,0x00,0x20,0x38,0x60,0x00,0x0A,0x48,0x33,0xB8,0xAD,0x7F,0xA3,0xEB,0x78,0x7F,0xC4,0xF3,0x78,0x4B,0xFF,0xFF,0x09,0x2C,0x03,0x00,0x00 };
uint8 smash4_softlockFixV0Mask[] =      { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF };

uint8 mh3u_raceConditionWorkaroundV0Signature[] = { 0x38,0x21,0x00,0x40,0x38,0x60,0x00,0x00,0x4E,0x80,0x00,0x20,0x80,0x7B,0xDB,0x9C,0x48,0x11,0x6B,0x81,0x2C,0x03,0x00,0x00 };
uint8 mh3u_raceConditionWorkaroundV0Mask[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF };

uint8 pmcs_yellowPaintStarCrashV0Signature[] = { 0x94,0x21,0xFF,0xB8,0xBE,0x61,0x00,0x14,0x7C,0x08,0x02,0xA6,0x7C,0xB3,0x2B,0x78,0x90,0x01,0x00,0x4C,0x7C,0x9D,0x23,0x78,0x83,0x3D,0x00,0x0C,0x81,0x39,0x04,0xA8,0x54,0x67,0x10,0x3A,0x7F,0xC7,0x48,0x2E,0x83,0x1E,0x00,0xDC,0x82,0xF8,0x00,0x08,0x2C,0x17,0x00,0x00 };
//uint8 mh3u_raceConditionWorkaroundV0Mask[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF };

uint8 bayo2_audioQueueFixSignature[] = { 0x80,0x03,0x00,0x3C,0x81,0x43,0x00,0x5C,0x81,0x83,0x00,0x40,0x55,0x48,0xB2,0xBE,0x3D,0x40,0x10,0x1D,0x7D,0x6C,0x42,0x14,0x39,0x4A,0x46,0xF0,0x7D,0x8B,0x00,0x50 };
uint8 bayo2_audioQueueFixMask[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0xFF,0xFF,0xFF,0xFF };

uint8 sm3dw_dynFrameBufferResScale[] = { 0x94,0x21,0xFF,0xB8,0xBF,0x21,0x00,0x2C,0x7C,0x08,0x02,0xA6,0x90,0x01,0x00,0x4C,0x7C,0x7E,0x1B,0x78,0x81,0x7E,0x07,0xD8,0x38,0x80,0x00,0x02,0x38,0x6B,0x00,0x03 };

uint8 tww_waitFunc[] = { 0x7C,0x08,0x02,0xA6,0x94,0x21,0xFF,0xF0,0x93,0xE1,0x00,0x0C,0x7C,0x7F,0x1B,0x78,0x90,0x01,0x00,0x14,0x80,0x7F,0x02,0xE0,0x81,0x83,0x00,0x0C,0x80,0x0C,0x00,0x1C,0x7C,0x09,0x03,0xA6,0x38,0xA0,0x00,0x00,0x38,0x9F,0x03,0x68 };

static_assert(sizeof(xcx_enterCriticalSectionSignature) == sizeof(xcx_enterCriticalSectionMask), "xcx_enterCriticalSection signature and size mismatch");
static_assert(sizeof(bayo2_audioQueueFixSignature) == sizeof(bayo2_audioQueueFixMask), "bayo2_audioQueueFix signature and size mismatch");

uint8 cars3_avro_schema_incref[] = { 0x2C,0x03,0x00,0x00,0x94,0x21,0xFF,0xE8,0x41,0x82,0x00,0x40,0x39,0x03,0x00,0x08,0x39,0x41,0x00,0x08,0x91,0x01,0x00,0x08,0x7D,0x80,0x50,0x28,0x2C,0x0C,0xFF,0xFF,0x41,0x82,0x00,0x28,0x39,0x21,0x00,0x0C,0x38,0x0C,0x00,0x01,0x38,0xE0,0x00,0x01,0x91,0x01,0x00,0x0C,0x7C,0x00,0x49,0x2D };

// For USA titles: 000500301001610a / 000500301001410a
uint8 miiverse_eshop_url_match_whitelist_func[] = {
	// For both titles, the code fully matches with the relative
	// branch targets, even if the absolute targets are different.
	0x89,0x45,0x00,0x00, // lbz r10, 0x0(r5)
	0x2c,0x0a,0x00,0x2e, // cmpwi r10, 0x2e
	0x40,0x82,0x00,0x08, // bne LAB_020ff3a8
	0x4b,0xff,0xff,0x5c // b FUN_020ff300
};
uint8 wave_libopenssl_ssl_verify_cert_chain[] = { 0x94,0x21,0xff,0x58,0x93,0xc1,0x00,0xa0,0x93,0xe1,0x00,0xa4,0x7c,0x9f,0x23,0x79,0x7c,0x7e,0x1b,0x78,0x90,0x01,0x00,0xac,0x41,0x82,0x00,0x14,0x7f,0xe3,0xfb,0x78 }; // rpl function for the above applets

sint32 hleIndex_h000000001 = -1;
sint32 hleIndex_h000000002 = -1;
sint32 hleIndex_h000000003 = -1;
sint32 hleIndex_h000000004 = -1;

/*
 * Returns true for all HLE functions that do not jump to LR
 * Used by recompiler to determine function code flow
 */
bool GamePatch_IsNonReturnFunction(uint32 hleIndex)
{
	if (hleIndex == hleIndex_h000000001)
		return true;
	if (hleIndex == hleIndex_h000000002)
		return true;
	if (hleIndex == hleIndex_h000000003)
		return false;
	if (hleIndex == hleIndex_h000000004)
		return false;
	return false;
}

static void GamePatch_PreserveInfinityLuminanceBits()
{
	const uint64 title = CafeSystem::GetForegroundTitleId();
	const uint16 version = CafeSystem::GetForegroundTitleVersion();
	const uint32 hash = CafeSystem::GetRPXHashUpdated();
	const bool infinity1 = title == 0x0005000010132900ULL && version == 49 && hash == 0xfc20d0eb;
	const bool infinity2 = title == 0x000500001016dc00ULL && version == 80 && hash == 0x2794ee69;
	if (!infinity1 && !infinity2)
		return;

	// These are all direct callers of this float byte-swap specialization in the
	// verified RPXs. Transport its argument in r3 so FTZ cannot erase GPU bytes.
	struct CallSite { uint32 address; uint32 load; uint32 branch; };
	const std::array<CallSite, 2> calls = infinity1 ?
		std::array<CallSite, 2>{{{0x03601490, 0xc03d0000, 0x48094a79}, {0, 0, 0}}} :
		std::array<CallSite, 2>{{{0x03701a50, 0xc03f0000, 0x48092229}, {0x03701c44, 0xc03d0000, 0x48092035}}};
	const uint32 swapFunction = infinity1 ? 0x03695f0c : 0x03793c7c;
	constexpr std::array<uint32, 11> expectedSwap{
		0x9421ffe8, 0xd021000c, 0x8181000c, 0x558b423e, 0x518bc00e,
		0x5580c42e, 0x5160061e, 0x90010008, 0xc0210008, 0x38210018, 0x4e800020};
	bool matches = true;
	for (const auto& call : calls)
	{
		if (call.address)
			matches &= memory_readU32(call.address) == call.load && memory_readU32(call.address + 4) == call.branch;
	}
	for (size_t i = 0; i < expectedSwap.size(); ++i)
		matches &= memory_readU32(swapFunction + static_cast<uint32>(4 * i)) == expectedSwap[i];
	if (!matches)
	{
		cemuLog_log(LogType::Force, "Disney Infinity: luminance byte-swap signature mismatch; patch skipped");
		return;
	}
	for (const auto& call : calls)
	{
		if (call.address)
			memory_writeU32(call.address, 0x80600000 | (call.load & 0x001fffff)); // lwz r3, source
	}
	memory_writeU32(swapFunction + 4, 0x9061000c); // stw r3, 12(r1), before the existing integer swap
	cemuLog_log(LogType::Force, "Disney Infinity {}: preserve luminance byte-swap bits", infinity1 ? 1 : 2);
}

namespace
{
	// Sonic recycles job nodes immediately. Value-only STWCX emulation permits
	// an ABA pop to restore an already-owned node and lose queued work. Keep all
	// three Rsdx list operations indivisible, without changing job/event semantics.
	std::mutex g_sonicAtomicListMutex;

	void SonicAtomicListReturn(PPCInterpreter_t* hCPU, uint32 previous)
	{
		hCPU->reservedMemAddr = 0;
		hCPU->reservedMemValue = 0;
		ppc_setCRBit(hCPU, CR_BIT_LT, 0);
		ppc_setCRBit(hCPU, CR_BIT_GT, 0);
		ppc_setCRBit(hCPU, CR_BIT_EQ, 1);
		ppc_setCRBit(hCPU, CR_BIT_SO, hCPU->xer_so);
		osLib_returnFromFunction(hCPU, previous);
	}

	void SonicAtomicListPush(PPCInterpreter_t* hCPU)
	{
		uint32 previous;
		{
			std::lock_guard lock(g_sonicAtomicListMutex);
			const uint32 head = hCPU->gpr[3];
			const uint32 entry = hCPU->gpr[4];
			previous = memory_readU32(head);
			memory_writeU32(entry, previous);
			memory_writeU32(head, entry);
		}
		SonicAtomicListReturn(hCPU, previous);
	}

	void SonicAtomicListPop(PPCInterpreter_t* hCPU)
	{
		uint32 previous;
		{
			std::lock_guard lock(g_sonicAtomicListMutex);
			const uint32 head = hCPU->gpr[3];
			previous = memory_readU32(head);
			memory_writeU32(head, previous ? memory_readU32(previous) : 0);
		}
		SonicAtomicListReturn(hCPU, previous);
	}

	void SonicAtomicListExchange(PPCInterpreter_t* hCPU)
	{
		uint32 previous;
		{
			std::lock_guard lock(g_sonicAtomicListMutex);
			const uint32 head = hCPU->gpr[3];
			previous = memory_readU32(head);
			memory_writeU32(head, hCPU->gpr[4]);
		}
		SonicAtomicListReturn(hCPU, previous);
	}

	constexpr std::array<uint32, 19> kSonicListPushCode{
		0x7c0802a6, 0x9421fff0, 0x93c10008, 0x93e1000c, 0x7c9f2378,
		0x7c7e1b78, 0x90010014, 0x4838f775, 0x7c60f028, 0x907f0000,
		0x7c00f06c, 0x7fe0f12d, 0x4082fff0, 0x80010014, 0x83c10008,
		0x7c0803a6, 0x83e1000c, 0x38210010, 0x4e800020};
	constexpr std::array<uint32, 10> kSonicListPopCode{
		0x7d801828, 0x2c0c0000, 0x7d806378, 0x41820008, 0x800c0000,
		0x7c00186c, 0x7c00192d, 0x4082ffe4, 0x7d836378, 0x4e800020};
	constexpr std::array<uint32, 6> kSonicListExchangeCode{
		0x7c6c1b78, 0x7c606028, 0x7c00606c, 0x7c80612d, 0x4082fff4, 0x4e800020};

	void PatchSonicAtomicLists()
	{
		if (CafeSystem::GetForegroundTitleId() != 0x0005000010128f00ull ||
			CafeSystem::GetForegroundTitleVersion() != 48 || CafeSystem::GetRPXHashUpdated() != 0x78278a63)
			return;
		auto matches = [](uint32 address, const auto& expected) {
			for (size_t i = 0; i < expected.size(); ++i)
			{
				const uint32 instructionAddress = address + static_cast<uint32>(i * 4);
				const uint32 actual = memory_readU32(instructionAddress);
				// The loader relocates this OSCoherencyBarrier import (REL24),
				// possibly through an absolute trampoline. Keep the BL opcode check.
				const uint32 mask = instructionAddress == 0x036a74f4 ? 0xfc000001 : 0xffffffff;
				if ((actual & mask) != (expected[i] & mask))
				{
					cemuLog_log(LogType::Force, "Sonic Lost World: atomic-list mismatch at {:08x}: {:08x}, expected {:08x}, mask {:08x}",
						instructionAddress, actual, expected[i], mask);
					return false;
				}
			}
			return true;
		};
		if (!matches(0x036a74d8, kSonicListPushCode) || !matches(0x036a7524, kSonicListPopCode) ||
			!matches(0x036a754c, kSonicListExchangeCode))
		{
			cemuLog_log(LogType::Force, "Sonic Lost World: atomic-list signatures mismatch; patch skipped");
			return;
		}
		const std::pair<uint32, void(*)(PPCInterpreter_t*)> hooks[]{
			{0x036a74d8, SonicAtomicListPush}, {0x036a7524, SonicAtomicListPop},
			{0x036a754c, SonicAtomicListExchange}};
		for (const auto& [address, function] : hooks)
			memory_writeU32(address, 0x48000000 | PPC_OPC_AA | PPCInterpreter_makeCallableExportDepr(function));
		cemuLog_log(LogType::Force, "Sonic Lost World: ABA-safe atomic job lists enabled");
	}
}

void GamePatch_scan()
{
	MPTR hleAddr;
	uint32 hleInstallStart = GetTickCount();
	GamePatch_PreserveInfinityLuminanceBits();
	PatchSonicAtomicLists();
	PatchWindWakerDepthPeek();
	PatchRiseMovieBuffers();
	PatchWiiFitCursorBounds();
	PatchZombiIntroDecode();
	if (CafeSystem::GetForegroundTitleId() == 0x0005000010142000ull &&
		CafeSystem::GetForegroundTitleVersion() == 0 && CafeSystem::GetRPXHashUpdated() == 0x95fd128b &&
		memory_readU32(0x02062dd8) == 0x4801b0ed && memory_readU32(0x04e49e34) == 0x81830494 &&
		memory_readU32(0x02062ddc) == 0x2c030000 && memory_readU32(0x02062de0) == 0x4082ffb4)
	{
		memory_writeU32(0x02062dd8, 0x48000000 | PPC_OPC_LK | PPC_OPC_AA | PPCInterpreter_makeCallableExportDepr(WatchDogsPendingRequests));
	}
	if (CafeSystem::GetForegroundTitleId() == 0x0005000010142000ull)
	{
		std::array<uint32_t, 7> words{};
		for (size_t i = 0; i < words.size(); ++i)
			words[i] = memory_readU32(WatchDogsVisibility::Addresses[i]);
		if (WatchDogsVisibility::Matches(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(), words))
		{
			memory_writeU32(0x057fd8d8, 0x3be00000);
			cemuLog_log(LogType::Force, "Watch Dogs: using engine CPU visibility jobs");
		}
	}

	if (CafeSystem::GetForegroundTitleId() == 0x0005000010209900ull &&
		CafeSystem::GetForegroundTitleVersion() == 16 && CafeSystem::GetRPXHashUpdated() == 0x09ff79b0)
	{
		// _bind_scene_lum_avg_param copies little-endian GPU data through LFS/STFS
		// before swapping it. Recompiler FTZ loses subnormal intermediate bit patterns.
		// Use an integer byte swap, preserving the exact luminance rather than clamping it.
		constexpr uint32 address = 0x02a6ab80;
		constexpr std::array<uint32, 4> expected{0xc1bf0000, 0x38610008, 0xd1a10008, 0x4805c901};
		constexpr std::array<uint32, 4> replacement{0x7c00fc2c, 0x38610008, 0x90010008, 0x60000000};
		bool matches = true;
		for (size_t i = 0; i < expected.size(); ++i)
			matches &= memory_readU32(address + static_cast<uint32>(i * 4)) == expected[i];
		if (matches)
		{
			for (size_t i = 0; i < replacement.size(); ++i)
				memory_writeU32(address + static_cast<uint32>(i * 4), replacement[i]);
			cemuLog_log(LogType::Force, "Cars 3: preserve luminance byte-swap bits");
		}
		else
			cemuLog_log(LogType::Force, "Cars 3: luminance byte-swap signature mismatch; patch skipped");
	}

	if (CafeSystem::GetForegroundTitleId() == 0x00050000101ac900ULL &&
		CafeSystem::GetForegroundTitleVersion() == 80 && CafeSystem::GetRPXHashUpdated() == 0x9ea42dbc)
	{
		// As in Cars 3, transport the GPU readback bits without interpreting the
		// little-endian intermediate as a float subject to recompiler FTZ.
		constexpr uint32 address = 0x03604554;
		constexpr std::array<uint32, 4> expected{0xc1bf0000, 0x38610008, 0xd1a10008, 0x48090445};
		constexpr std::array<uint32, 4> replacement{0x7c00fc2c, 0x38610008, 0x90010008, 0x60000000};
		bool matches = true;
		for (size_t i = 0; i < expected.size(); ++i)
			matches &= memory_readU32(address + static_cast<uint32>(i * 4)) == expected[i];
		if (matches)
		{
			for (size_t i = 0; i < replacement.size(); ++i)
				memory_writeU32(address + static_cast<uint32>(i * 4), replacement[i]);
			cemuLog_log(LogType::Force, "Disney Infinity 3.0: preserve luminance byte-swap bits");
		}
		else
			cemuLog_log(LogType::Force, "Disney Infinity 3.0: luminance byte-swap signature mismatch; patch skipped");
	}

	hleAddr = hle_locate(xcx_gpuHangDetection_degradeFramebuffer, NULL, sizeof(xcx_gpuHangDetection_degradeFramebuffer));
	if( hleAddr )
	{
#ifdef CEMU_DEBUG_ASSERT
		cemuLog_log(LogType::Force, "HLE: XCX GPU hang detection");
#endif
		// remove the ADDI r25, r25, 1 instruction
		memory_writeU32(hleAddr, memory_readU32(hleAddr+4));
	}

	hleAddr = hle_locate(xcx_framebufferReductionSignature, xcx_framebufferReductionMask, sizeof(xcx_framebufferReductionSignature));
	if( hleAddr )
	{
#ifdef CEMU_DEBUG_ASSERT
		cemuLog_log(LogType::Force, "HLE: Prevent XCX rendertarget reduction");
#endif
		uint32 bl = memory_readU32(hleAddr+0x14);
		uint32 func_isReductionBuffer = hleAddr + 0x14 + (bl&0x3FFFFFC);

		// patch isReductionBuffer
		memory_writeU32(func_isReductionBuffer, 0x38600000); // LI R3, 0
		memory_writeU32(func_isReductionBuffer+4, 0x4E800020); // BLR

	}

	hleIndex_h000000001 = osLib_getFunctionIndex("hle", "h000000001");
	hleAddr = hle_locate(botw_busyLoopSignature, botw_busyLoopMask, sizeof(botw_busyLoopSignature));
	if (hleAddr)
	{
#ifdef CEMU_DEBUG_ASSERT
		cemuLog_log(LogType::Force, "HLE: Patch BotW busy loop 1 at 0x{:08x}", hleAddr);
#endif
		sint32 functionIndex = hleIndex_h000000001;
		uint32 opcode = (1 << 26) | (functionIndex); // opcode for HLE: 0x1000 + FunctionIndex
		memory_write<uint32>(hleAddr - 4, opcode);
	}
	hleIndex_h000000002 = osLib_getFunctionIndex("hle", "h000000002");
	hleAddr = hle_locate(botw_busyLoopSignature2, botw_busyLoopMask2, sizeof(botw_busyLoopSignature2));
	if (hleAddr)
	{
#ifdef CEMU_DEBUG_ASSERT
		cemuLog_log(LogType::Force, "HLE: Patch BotW busy loop 2 at 0x{:08x}", hleAddr);
#endif
		sint32 functionIndex = hleIndex_h000000002;
		uint32 opcode = (1 << 26) | (functionIndex); // opcode for HLE: 0x1000 + FunctionIndex
		memory_write<uint32>(hleAddr - 4, opcode);
	}

	// FFL library float array endian conversion
	// original function needs invalid float values to remain intact between LFSX -> STFSX, which is not supported in recompiler mode
	hleIndex_h000000003 = osLib_getFunctionIndex("hle", "h000000003");
	hleAddr = hle_locate(ffl_floatArrayEndianSwap, NULL, sizeof(ffl_floatArrayEndianSwap));
	if (hleAddr)
	{
		cemuLog_logDebug(LogType::Force, "HLE: Hook FFL float array endian swap function (v1) at 0x{:08x}", hleAddr);
		sint32 functionIndex = hleIndex_h000000003;
		uint32 opcode = (1 << 26) | (functionIndex); // opcode for HLE: 0x1000 + FunctionIndex
		memory_write<uint32>(hleAddr, opcode);
	}
	else
	{
		// Try v2 signature (Mario Kart 8 and possibly other titles with reordered prologue)
		hleAddr = hle_locate(ffl_floatArrayEndianSwap_v2, NULL, sizeof(ffl_floatArrayEndianSwap_v2));
		if (hleAddr)
		{
			cemuLog_logDebug(LogType::Force, "HLE: Hook FFL float array endian swap function (v2) at 0x{:08x}", hleAddr);
			sint32 functionIndex = hleIndex_h000000003;
			uint32 opcode = (1 << 26) | (functionIndex); // opcode for HLE: 0x1000 + FunctionIndex
			memory_write<uint32>(hleAddr, opcode);
		}
	}

	// XCX freeze workaround
	//hleAddr = hle_locate(xcx_enterCriticalSectionSignature, xcx_enterCriticalSectionMask, sizeof(xcx_enterCriticalSectionSignature));
	//if (hleAddr)
	//{
	//	cemuLog_logDebug(LogType::Force, "HLE: Hook XCX enterCriticalSection function at 0x{:08x}", hleAddr);
	//	hleIndex_h000000004 = osLib_getFunctionIndex("hle", "h000000004");
	//	sint32 functionIndex = hleIndex_h000000004;
	//	uint32 opcode = (1 << 26) | (functionIndex); // opcode for HLE: 0x1000 + FunctionIndex
	//	memory_writeU32Direct(hleAddr, opcode);
	//}

	// MH3U race condition (tested for EU+US 1.2)
	hleAddr = hle_locate(mh3u_raceConditionWorkaroundV0Signature, mh3u_raceConditionWorkaroundV0Mask, sizeof(mh3u_raceConditionWorkaroundV0Mask));
	if (hleAddr)
	{
		uint32 patchAddr = hleAddr + 0x10;
		cemuLog_log(LogType::Force, "HLE: Patch MH3U race condition candidate at 0x{:08x}", patchAddr);
		uint32 funcAddr = PPCInterpreter_makeCallableExportDepr(hleExport_mh3u_raceConditionWorkaround);
		// set absolute jump
		uint32 opc = 0x48000000;
		opc |= PPC_OPC_LK;
		opc |= PPC_OPC_AA;
		opc |= funcAddr;
		memory_writeU32(patchAddr, opc);
	}

	// Super Smash Bros softlock fix
	// fixes random softlocks that can occur after matches
	hleAddr = hle_locate(smash4_softlockFixV0Signature, smash4_softlockFixV0Mask, sizeof(smash4_softlockFixV0Signature));
	if (hleAddr)
	{
		cemuLog_logDebug(LogType::Force, "Smash softlock fix: 0x{:08x}", hleAddr);
		memory_writeU32(hleAddr+0x20, memory_readU32(hleAddr+0x1C));
	}

	// Color Splash Yellow paint star crash workaround
	// fixes the crash at the beginning of the dream sequence cutscene after collecting the yellow paint star
	hleAddr = hle_locate(pmcs_yellowPaintStarCrashV0Signature, nullptr, sizeof(pmcs_yellowPaintStarCrashV0Signature));
	if (hleAddr)
	{
		cemuLog_logDebug(LogType::Force, "Color Splash crash fix: 0x{:08x}", hleAddr);
		uint32 funcAddr = PPCInterpreter_makeCallableExportDepr(hleExport_pmcs_yellowPaintStarCrashWorkaround);
		// set absolute jump
		uint32 opc = 0x48000000;
		opc |= PPC_OPC_LK;
		opc |= PPC_OPC_AA;
		opc |= funcAddr;
		memory_writeU32(hleAddr+0x20, opc);
	}

	// Bayonetta 2 sound queue patch (fixes audio starting to loop infinitely when there is stutter)
	hleAddr = hle_locate(bayo2_audioQueueFixSignature, bayo2_audioQueueFixMask, sizeof(bayo2_audioQueueFixSignature));
	if (hleAddr)
	{
		// replace CMPL with CMP
		cemuLog_log(LogType::Force, "Patching Bayonetta 2 audio bug at: 0x{:08x}", hleAddr+0x34);
		uint32 opc = memory_readU32(hleAddr + 0x34);
		opc &= ~(0x3FF << 1); // turn CMPL to CMP
		memory_writeU32(hleAddr + 0x34, opc);
	}

	if (CafeSystem::GetRPXHashUpdated() == 0xb1c033dd) // Wind Waker US
	{
		uint32 p = memory_readU32(0x02813878);
		if (p == 0x40800018)
		{
			debug_printf("HLE: TWW US dsp kill channel patch\n");
			uint32 li = 0x18;
			uint32 opcode = (li & 0x3FFFFFC) | (18 << 26); // replace BGE with B instruction
			memory_writeU32(0x02813878, opcode);
		}
	}
	else if (CafeSystem::GetRPXHashUpdated() == 0xCDC68ACD) // Wind Waker EU
	{
		uint32 p = memory_readU32(0x2814138);
		if (p == 0x40800018)
		{
			debug_printf("HLE: TWW EU dsp kill channel patch\n");
			uint32 li = 0x18;
			uint32 opcode = (li & 0x3FFFFFC) | (18 << 26); // replace BGE with B instruction
			memory_writeU32(0x02814138, opcode);
		}
	}

	if (CafeSystem::GetForegroundTitleId() == 0x0005000010110200ull &&
		CafeSystem::GetForegroundTitleVersion() == 32 &&
		CafeSystem::GetRPXHashUpdated() == 0x46e6716d &&
		memory_readU32(0x020c23a4) == 0x4bfffcf1 &&
		memory_readU32(0x020c2094) == 0xfd600890 &&
		memory_readU32(0x020c20a0) == 0xeda05830)
	{
		const auto hook = PPCInterpreter_makeCallableExportDepr(hleExport_orochiHitLightDivide);
		memory_writeU32(0x020c23a4, 0x48000000 | PPC_OPC_AA | PPC_OPC_LK | hook);
		cemuLog_log(LogType::Force, "Warriors Orochi 3: preserving empty hit light across zero-divisor initialization");
	}

	// Mighty No. 9 v16 can publish invalid render-resource state on its render thread.
	if (CafeSystem::GetForegroundTitleId() == 0x00050000101C9600ull &&
		CafeSystem::GetForegroundTitleVersion() == 16 &&
		CafeSystem::GetRPXHashUpdated() == 0x73E4FFC8)
	{
		s_mn9DeferredCleanupBatch = {};

		// Keep the original exit behavior, but retain the initiating caller before
		// the Game Thread unwinds and the main thread performs final shutdown.
		constexpr uint32 kAppRequestExitAddress = 0x02E8E008;
		constexpr uint32 kExpectedAppRequestExitFirstInstruction = 0x7C0802A6; // mflr r0
		const uint32 appRequestExitFirstInstruction = memory_readU32(kAppRequestExitAddress);
		if (appRequestExitFirstInstruction == kExpectedAppRequestExitFirstInstruction)
		{
			const uint32 funcAddr = PPCInterpreter_makeCallableExportDepr(hleExport_mn9_appRequestExitTrace);
			const uint32 opc = 0x48000000 | PPC_OPC_AA | funcAddr;
			memory_writeU32(kAppRequestExitAddress, opc);
			cemuLog_log(LogType::Force, "Mighty No. 9: tracing appRequestExit callers");
		}
		else
		{
			cemuLog_log(LogType::Force,
				"Mighty No. 9: appRequestExit trace signature mismatch (instruction=0x{:08x})",
				appRequestExitFirstInstruction);
		}

		// UE3 defers render-resource destruction by one cleanup batch. Give MN9 one
		// additional native render fence because Cemu can retire its releases late.
		constexpr uint32 kPendingCleanupCallAddress = 0x02EBE138;
		constexpr uint32 kExpectedPendingCleanupCall = 0x4B462E0D; // bl FPendingCleanupObjects::~FPendingCleanupObjects
		const uint32 pendingCleanupCall = memory_readU32(kPendingCleanupCallAddress);
		if (pendingCleanupCall == kExpectedPendingCleanupCall)
		{
			const uint32 funcAddr = PPCInterpreter_makeCallableExportDepr(hleExport_mn9_pendingCleanupGuard);
			const uint32 opc = 0x48000000 | PPC_OPC_LK | PPC_OPC_AA | funcAddr;
			memory_writeU32(kPendingCleanupCallAddress, opc);
			cemuLog_log(LogType::Force, "Mighty No. 9: deferred render cleanup by one fence");
		}
		else
		{
			cemuLog_log(LogType::Force,
				"Mighty No. 9: deferred-cleanup signature mismatch (call=0x{:08x})",
				pendingCleanupCall);
		}

		// Treat the proxy sentinel 1 like null instead of dereferencing guest address 0x69.
		constexpr uint32 kProxyCompareAddress = 0x02AFDF84;
		constexpr uint32 kProxyBranchAddress = 0x02AFDF8C;
		constexpr uint32 kExpectedCompare = 0x2C060000; // cmpwi r6, 0
		constexpr uint32 kExpectedBranch = 0x418200F4; // beq 0x02afe080
		const uint32 proxyCompare = memory_readU32(kProxyCompareAddress);
		const uint32 proxyBranch = memory_readU32(kProxyBranchAddress);
		if (proxyCompare == kExpectedCompare && proxyBranch == kExpectedBranch)
		{
			memory_writeU32(kProxyCompareAddress, 0x28060001); // cmplwi r6, 1
			memory_writeU32(kProxyBranchAddress, 0x408100F4); // ble 0x02afe080
			cemuLog_log(LogType::Force, "Mighty No. 9: guarded invalid motion-blur proxy");
		}
		else
		{
			cemuLog_log(LogType::Force,
				"Mighty No. 9: motion-blur guard signature mismatch (compare=0x{:08x}, branch=0x{:08x})",
				proxyCompare, proxyBranch);
		}

		// Guard the exact BeginInitResource virtual call that can receive an out-of-range target.
		constexpr uint32 kInitResourceCallAddress = 0x02337010;
		constexpr uint32 kExpectedInitResourceCall = 0x4E800421; // bctrl
		const uint32 initResourceCall = memory_readU32(kInitResourceCallAddress);
		if (initResourceCall == kExpectedInitResourceCall)
		{
			const uint32 funcAddr = PPCInterpreter_makeCallableExportDepr(hleExport_mn9_initResourceGuard);
			uint32 opc = 0x48000000;
			opc |= PPC_OPC_LK;
			opc |= PPC_OPC_AA;
			opc |= funcAddr;
			memory_writeU32(kInitResourceCallAddress, opc);
			cemuLog_log(LogType::Force, "Mighty No. 9: guarded BeginInitResource virtual call");
		}
		else
		{
			cemuLog_log(LogType::Force,
				"Mighty No. 9: BeginInitResource guard signature mismatch (call=0x{:08x})",
				initResourceCall);
		}

		// A stale queued transform command can retain a scene-info object whose proxy was recycled.
		constexpr uint32 kUpdateTransformRemoveCallAddress = 0x02B10934;
		constexpr uint32 kExpectedUpdateTransformRemoveCall = 0x4BF8481D; // bl FPrimitiveSceneInfo::RemoveFromScene
		const uint32 updateTransformRemoveCall = memory_readU32(kUpdateTransformRemoveCallAddress);
		if (updateTransformRemoveCall == kExpectedUpdateTransformRemoveCall)
		{
			const uint32 funcAddr = PPCInterpreter_makeCallableExportDepr(hleExport_mn9_updateTransformGuard);
			uint32 opc = 0x48000000;
			opc |= PPC_OPC_LK;
			opc |= PPC_OPC_AA;
			opc |= funcAddr;
			memory_writeU32(kUpdateTransformRemoveCallAddress, opc);
			cemuLog_log(LogType::Force, "Mighty No. 9: guarded stale queued transform commands");
		}
		else
		{
			cemuLog_log(LogType::Force,
				"Mighty No. 9: queued-transform guard signature mismatch (call=0x{:08x})",
				updateTransformRemoveCall);
		}

		// RemoveFromScene assumes its scene-map lookup always succeeds and otherwise
		// passes null to a helper that immediately dereferences it. An absent entry is
		// already removed, so finish through the function's normal epilogue.
		constexpr uint32 kRemoveFromSceneMissingEntryAddress = 0x02A951D8;
		constexpr uint32 kExpectedRemoveFromSceneMissingEntry = 0x80FC00D8; // lwz r7, 216(r28)
		constexpr uint32 kRemoveFromSceneEpilogueBranch = 0x48000384; // b 0x02a9555c
		const uint32 removeFromSceneMissingEntry = memory_readU32(kRemoveFromSceneMissingEntryAddress);
		if (removeFromSceneMissingEntry == kExpectedRemoveFromSceneMissingEntry)
		{
			memory_writeU32(kRemoveFromSceneMissingEntryAddress, kRemoveFromSceneEpilogueBranch);
			cemuLog_log(LogType::Force, "Mighty No. 9: guarded missing RemoveFromScene entries");
		}
		else
		{
			cemuLog_log(LogType::Force,
				"Mighty No. 9: RemoveFromScene guard signature mismatch (instruction=0x{:08x})",
				removeFromSceneMissingEntry);
		}
	}

	// disable SM3DW dynamic resolution scaling (fixes level 1-5 spamming lots of texture creates when gradually resizing framebuffer)
	hleAddr = hle_locate(sm3dw_dynFrameBufferResScale, nullptr, sizeof(sm3dw_dynFrameBufferResScale));
	if (hleAddr)
	{
		cemuLog_log(LogType::Force, "Patching SM3DW dynamic resolution scaling at: 0x{:08x}", hleAddr);
		memory_writeU32(hleAddr, 0x4E800020); // BLR
	}

	// remove unnecessary lock from a wait function in TWW
	// this resolves a deadlock in singlecore mode
	hleAddr = hle_locate(tww_waitFunc, nullptr, sizeof(tww_waitFunc));
	if (hleAddr)
	{
		cemuLog_log(LogType::Force, "Patching TWW race conditon at: 0x{:08x}", hleAddr);
		// NOP calls to Lock/Unlock mutex
		memory_writeU32(hleAddr + 0x34, 0x60000000);
		memory_writeU32(hleAddr + 0x48, 0x60000000);
		memory_writeU32(hleAddr + 0x50, 0x60000000);
		memory_writeU32(hleAddr + 0x64, 0x60000000);
	}

	// Patch function in Miiverse/eShop wave.rpx that matches
	// a domain against another to validate its whitelist.
	// This allows those browsers to load any domain.
	const NetworkService service = ActiveSettings::GetNetworkService();
	if (service != NetworkService::Nintendo // Only patch for custom services.
		&& CafeSystem::GetForegroundTitleArgStr().ends_with("wave.rpx")
		&& (hleAddr = hle_locate(miiverse_eshop_url_match_whitelist_func,
			nullptr, sizeof(miiverse_eshop_url_match_whitelist_func))))
	{
		cemuLog_log(LogType::Force, "Patching Miiverse/eShop whitelist check at: 0x{:08x}", hleAddr);
		// Always return 1. (Note that the matched pattern is not at the beginning but still works)
		memory_writeU32(hleAddr, 0x38600001);
		memory_writeU32(hleAddr + 0x4, 0x4e800020);
		// Note that the same applies to Account Settings, but
		// its version of this function differs a lot.
		// Search: 88 0c ff ff 2c 00 00 2e (lbz r0,-0x1(r12); cmpwi r0,0x2e)
	}

	// Additionally patch out SSL checks for libopenssl.rpl, used by the browser.
	if (IsNetworkServiceSSLDisabled(service)
		&& RPLLoader_GetHandleByModuleName("libopenssl.rpl") != RPL_INVALID_HANDLE
		&& (hleAddr = hle_locate(wave_libopenssl_ssl_verify_cert_chain,
			nullptr, sizeof(wave_libopenssl_ssl_verify_cert_chain))))
	{
		cemuLog_log(LogType::Force, "Patching OpenSSL ssl_verify_cert_chain at: 0x{:08x}", hleAddr);
		// Reference: https://github.com/PretendoNetwork/Meowth/blob/meowth/src/patcher/patches/webkit_applets.cpp
		memory_writeU32(hleAddr + 0x28, 0x60000000);
		memory_writeU32(hleAddr + 0x40, 0x38600001);
	}

	uint32 hleInstallEnd = GetTickCount();
	cemuLog_log(LogType::Force, "HLE scan time: {}ms", hleInstallEnd-hleInstallStart);
}

RunAtCemuBoot _loadGamePatchAPI([]()
	{
		osLib_addFunction("hle", "h000000001", hleExport_breathOfTheWild_busyLoop);
		osLib_addFunction("hle", "h000000002", hleExport_breathOfTheWild_busyLoop2);
		osLib_addFunction("hle", "h000000003", hleExport_ffl_swapEndianFloatArray);
		osLib_addFunction("hle", "h000000004", hleExport_xcx_enterCriticalSection);
	});
