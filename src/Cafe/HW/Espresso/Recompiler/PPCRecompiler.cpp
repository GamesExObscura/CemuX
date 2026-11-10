#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCFunctionBoundaryTracker.h"
#include "PPCRecompiler.h"
#include "ZombiIntroJit.h"
#include "WatchDogsMediaJit.h"
#include "PPCRecompilerIml.h"
#include "util/containers/RangeStore.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "Cafe/CafeSystem.h"
#include "Cemu/Debug/CompatibilityDiagnostics.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"
#include "util/helpers/fspinlock.h"
#include "util/helpers/helpers.h"
#include "util/MemMapper/MemMapper.h"

#include "IML/IML.h"
#include "IML/IMLRegisterAllocator.h"
#include "BackendX64/BackendX64.h"
#ifdef __aarch64__
#include "BackendAArch64/BackendAArch64.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"

#define PPCREC_FORCE_SYNCHRONOUS_COMPILATION	0 // if 1, then function recompilation will block and execute on the thread that called PPCRecompiler_visitAddressNoBlock
#define PPCREC_LOG_RECOMPILATION_RESULTS		0

struct ppcInvalidationRange
{
	MPTR startAddress;
	uint32 size;

	ppcInvalidationRange(MPTR _startAddress, uint32 _size) : startAddress(_startAddress), size(_size) {};
};

struct ppcRecompilerFuncRange
{
	MPTR	ppcStart;
	uint32  ppcSize;
	void*   x86Start;
	size_t  x86Size;
};

struct
{
	std::atomic_bool initialized{false};
	FSpinlock recompilerSpinlock;
	std::queue<MPTR> targetQueue;
	std::queue<MPTR> ac4DependencyQueue;
	std::set<MPTR> ac4PromotedDependencies;
	std::vector<ppcInvalidationRange> invalidationRanges;
	std::atomic_int_fast32_t recompilerEnableCount{0};
	// recompiler thread
	std::thread workerThread;
	std::atomic_bool workerThreadStopSignal{false};
	// function storage
	RangeStore<PPCRecFunction_t*, uint32, 7703, 0x2000> functionStorage;
}s_ppcRecompilerState;

void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
static std::mutex s_singleRecompilationMutex;
#endif

void PPCRecompiler_recompileAtAddress(uint32 address);

static bool PPCRecompiler_isOpeningDependency(uint32 address)
{
#if defined(ARCH_X86_64)
	if (ZombiIntroJit::IsDependency(CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(), address))
		return true;
	if (WatchDogsMediaJit::IsDependency(CafeSystem::GetForegroundTitleId(),
		CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(), address))
		return true;
	// This build's opening frame worker waits on these routines while the JIT FIFO is backlogged.
	if (address < 0x053a25e0 || address >= 0x053b2800 || (address & 3))
		return false;
	return CafeSystem::GetForegroundTitleId() == 0x0005000010138b00ull &&
		CafeSystem::GetForegroundTitleVersion() == 32 && CafeSystem::GetRPXHashUpdated() == 0x60b0d3a7;
#else
	return false;
#endif
}

// this function does never block and can fail if the recompiler lock cannot be acquired immediately
void PPCRecompiler_visitAddressNoBlock(uint32 enterAddress)
{
	// Jump-table entry zero is reserved as the interpreter/callback-exit sentinel.
	if (enterAddress == 0)
		return;

#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	s_ppcRecompilerState.recompilerSpinlock.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;
	s_ppcRecompilerState.recompilerSpinlock.unlock();
	s_singleRecompilationMutex.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] == PPCRecompiler_leaveRecompilerCode_visited)
	{
		PPCRecompiler_recompileAtAddress(enterAddress);
	}
	s_singleRecompilationMutex.unlock();
	return;
#endif
	// quick read-only check without lock
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	// try to acquire lock
	if (!s_ppcRecompilerState.recompilerSpinlock.try_lock())
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		// was visited since previous check
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	// add to recompilation queue and flag as visited
	s_ppcRecompilerState.targetQueue.emplace(enterAddress);
	// Promote only requested entries, once each; retain FIFO retries and bound the extra work.
	if (PPCRecompiler_isOpeningDependency(enterAddress) &&
		s_ppcRecompilerState.ac4PromotedDependencies.size() < 128 &&
		s_ppcRecompilerState.ac4PromotedDependencies.insert(enterAddress).second)
		s_ppcRecompilerState.ac4DependencyQueue.emplace(enterAddress);
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;

	s_ppcRecompilerState.recompilerSpinlock.unlock();
}

void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress)
{
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	PPCRecompiler_visitAddressNoBlock(enterAddress);
}

void PPCRecompiler_enter(PPCInterpreter_t* hCPU, PPCREC_JUMP_ENTRY funcPtr)
{
#if BOOST_OS_WINDOWS
	uint32 prevState = _controlfp(0, 0);
	_controlfp(_RC_NEAR, _MCW_RC);
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
	_controlfp(prevState, _MCW_RC);
	// debug recompiler exit - useful to find frequently executed functions which couldn't be recompiled
	#ifdef CEMU_DEBUG_ASSERT
	if (hCPU->remainingCycles > 0 && GetAsyncKeyState(VK_F4))
	{
		auto t = std::chrono::high_resolution_clock::now();
		auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
		cemuLog_log(LogType::Force, "Recompiler exit: 0x{:08x} LR: 0x{:08x} Timestamp {}.{:04}", hCPU->instructionPointer, hCPU->spr.LR, dur / 1000LL, (dur % 1000LL));
	}
	#endif
#else
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
#endif
	// after leaving recompiler prematurely attempt to recompile the code at the new location
	if (hCPU->remainingCycles > 0)
	{
		PPCRecompiler_visitAddressNoBlock(hCPU->instructionPointer);
	}
}

void PPCRecompiler_attemptEnterWithoutRecompile(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited && funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}

void PPCRecompiler_attemptEnter(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_ppcRecompilerState.recompilerEnableCount <= 0)
		return;
	if (hCPU->remainingCycles <= 0)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr == PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		PPCRecompiler_visitAddressNoBlock(enterAddress);
	}
	else if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		// enter
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}
bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext);

PPCRecFunction_t* PPCRecompiler_recompileFunction(PPCFunctionBoundaryTracker::PPCRange_t range, std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPointsOut, PPCFunctionBoundaryTracker& boundaryTracker)
{
	if (range.startAddress >= PPC_REC_CODE_AREA_END)
	{
		cemuLog_log(LogType::Force, "Attempting to recompile function outside of allowed code area");
		return nullptr;
	}
	uint32 codeGenRangeStart;
	uint32 codeGenRangeSize = 0;
	coreinit::OSGetCodegenVirtAddrRangeInternal(codeGenRangeStart, codeGenRangeSize);
	if (codeGenRangeSize != 0)
	{
		if (range.startAddress >= codeGenRangeStart && range.startAddress < (codeGenRangeStart + codeGenRangeSize))
		{
			if (coreinit::codeGenShouldAvoid())
			{
				return nullptr;
			}
		}
	}

	PPCRecFunction_t* ppcRecFunc = new PPCRecFunction_t();
	ppcRecFunc->ppcAddress = range.startAddress;
	ppcRecFunc->ppcSize = range.length;

#if PPCREC_LOG_RECOMPILATION_RESULTS
	BenchmarkTimer bt;
	bt.Start();
#endif

	// generate intermediate code
	ppcImlGenContext_t ppcImlGenContext = { 0 };
	ppcImlGenContext.debug_entryPPCAddress = range.startAddress;
	bool compiledSuccessfully = PPCRecompiler_generateIntermediateCode(ppcImlGenContext, ppcRecFunc, entryAddresses, boundaryTracker);
	if (compiledSuccessfully == false)
	{
		delete ppcRecFunc;
		return nullptr;
	}

	uint32 ppcRecLowerAddr = LaunchSettings::GetPPCRecLowerAddr();
	uint32 ppcRecUpperAddr = LaunchSettings::GetPPCRecUpperAddr();
	uint32 ppcRecExcludeLowerAddr = LaunchSettings::GetPPCRecExcludeLowerAddr();
	uint32 ppcRecExcludeUpperAddr = LaunchSettings::GetPPCRecExcludeUpperAddr();
	const bool hasExplicitExcludeRange = ppcRecExcludeLowerAddr != 0 || ppcRecExcludeUpperAddr != 0;
	if (CafeSystem::GetForegroundTitleId() == 0x0005000010106B00ull &&
		CafeSystem::GetForegroundTitleVersion() == 80 &&
		ppcRecExcludeLowerAddr == 0 && ppcRecExcludeUpperAddr == 0)
	{
		ppcRecExcludeLowerAddr = 0x042B5400;
		ppcRecExcludeUpperAddr = 0x042B8300;
	}
	else if (CafeSystem::GetForegroundTitleId() == 0x000500001011F300ull &&
		CafeSystem::GetForegroundTitleVersion() == 0 &&
		ppcRecExcludeLowerAddr == 0 && ppcRecExcludeUpperAddr == 0)
	{
		// The Walking Dead: Survival Instinct hangs after Start when GHS __gh_memclr32 is recompiled.
		ppcRecExcludeLowerAddr = 0x026B3250;
		ppcRecExcludeUpperAddr = 0x026B32AC;
	}
	else if (CafeSystem::GetForegroundTitleId() == 0x00050000101C9600ull &&
		CafeSystem::GetForegroundTitleVersion() == 16 &&
		ppcRecExcludeLowerAddr == 0 && ppcRecExcludeUpperAddr == 0)
	{
		// Mighty No. 9 loops while entering its title menu when the first RPX function is recompiled.
		ppcRecExcludeLowerAddr = 0x02000020;
		ppcRecExcludeUpperAddr = 0x02000058;
	}
	else if (!hasExplicitExcludeRange &&
		CafeSystem::GetForegroundTitleId() == 0x000500001010d900ull &&
		CafeSystem::GetForegroundTitleVersion() == 0 &&
		CafeSystem::GetRPXHashUpdated() == 0xb0b23fd7)
	{
		constexpr uint32 kByteSwapStart = 0x03413ee4;
		constexpr uint32 kByteSwapEnd = 0x03413f10;
		const uint64 rangeEnd = (uint64)ppcRecFunc->ppcAddress + ppcRecFunc->ppcSize;
		if (ppcRecFunc->ppcAddress < kByteSwapEnd && rangeEnd > kByteSwapStart)
		{
			// FIFA passes raw little-endian seat data through an FPR. Recompiled STFS
			// flushes temporary denormals to zero; the interpreter preserves their bits.
			constexpr uint32 signature[] = {
				0x9421ffe8, 0xd021000c, 0x8181000c, 0x558b423e, 0x518bc00e,
				0x5580c42e, 0x5160061e, 0x90010008, 0xc0210008, 0x38210018, 0x4e800020
			};
			bool matches = true;
			for (size_t i = 0; i < std::size(signature); ++i)
				matches &= memory_readU32(kByteSwapStart + static_cast<uint32>(i * 4)) == signature[i];
			if (matches)
			{
				ppcRecExcludeLowerAddr = kByteSwapStart;
				ppcRecExcludeUpperAddr = kByteSwapEnd;
			}
		}
	}

	if (!hasExplicitExcludeRange &&
		CafeSystem::GetForegroundTitleId() == 0x000500001010E400ull &&
		CafeSystem::GetForegroundTitleVersion() == 0 &&
		CafeSystem::GetRPXHashUpdated() == 0xEBA24F4A)
	{
		constexpr uint32 kTraversalStart = 0x0276221C;
		constexpr uint32 kTraversalEnd = 0x027648F4;
		const uint64 rangeEnd = (uint64)ppcRecFunc->ppcAddress + ppcRecFunc->ppcSize;
		if (ppcRecFunc->ppcAddress < kTraversalEnd && rangeEnd > kTraversalStart &&
			memory_readU32(kTraversalStart) == 0x7C0802A6 &&
			memory_readU32(kTraversalStart + 4) == 0x9421F470 &&
			memory_readU32(0x02764894) == 0x4082F60C &&
			memory_readU32(kTraversalEnd - 4) == 0x4E800020)
		{
			// Game Party's compiled traversal never completes after character confirmation.
			// Interpreting this routine preserves the values needed to finish the traversal.
			ppcRecExcludeLowerAddr = kTraversalStart;
			ppcRecExcludeUpperAddr = kTraversalEnd;
		}
	}

	if (ppcRecLowerAddr != 0 && ppcRecUpperAddr != 0)
	{
		if (ppcRecFunc->ppcAddress < ppcRecLowerAddr || ppcRecFunc->ppcAddress > ppcRecUpperAddr)
		{
			delete ppcRecFunc;
			return nullptr;
		}
	}
	if (ppcRecExcludeLowerAddr != 0 && ppcRecExcludeUpperAddr != 0)
	{
		const uint64 rangeEnd = (uint64)ppcRecFunc->ppcAddress + ppcRecFunc->ppcSize;
		if (ppcRecFunc->ppcAddress < ppcRecExcludeUpperAddr && rangeEnd > ppcRecExcludeLowerAddr)
		{
			if (CompatibilityDiagnostics::IsEnabled("recompiler", CafeSystem::GetForegroundTitleId()))
			{
				cemuLog_log(LogType::Force,
					"[COMPAT:RECOMPILER] event=excluded-function address=0x{:08x} size=0x{:x} end=0x{:08x} gate=0x{:08x}-0x{:08x}",
					ppcRecFunc->ppcAddress, ppcRecFunc->ppcSize, rangeEnd,
					ppcRecExcludeLowerAddr, ppcRecExcludeUpperAddr);
			}
			delete ppcRecFunc;
			return nullptr;
		}
	}
	if (!hasExplicitExcludeRange &&
		CafeSystem::GetForegroundTitleId() == 0x00050000101C9600ull &&
		CafeSystem::GetForegroundTitleVersion() == 16)
	{
		constexpr uint32 kCollisionCheckStart = 0x026F23B8;
		constexpr uint32 kCollisionCheckEnd = 0x026F3480;
		const uint64 rangeEnd = (uint64)ppcRecFunc->ppcAddress + ppcRecFunc->ppcSize;
		if (ppcRecFunc->ppcAddress < kCollisionCheckEnd && rangeEnd > kCollisionCheckStart)
		{
			if (CompatibilityDiagnostics::IsEnabled("recompiler", CafeSystem::GetForegroundTitleId()))
			{
				cemuLog_log(LogType::Force,
					"[COMPAT:RECOMPILER] event=excluded-function address=0x{:08x} size=0x{:x} end=0x{:08x} gate=mn9-collision-check",
					ppcRecFunc->ppcAddress, ppcRecFunc->ppcSize, rangeEnd);
			}
			delete ppcRecFunc;
			return nullptr;
		}
	}

	// apply passes
	if (!PPCRecompiler_ApplyIMLPasses(ppcImlGenContext))
	{
		delete ppcRecFunc;
		return nullptr;
	}
#if defined(ARCH_X86_64)
	// emit x64 code
	bool x64GenerationSuccess = PPCRecompiler_generateX64Code(ppcRecFunc, &ppcImlGenContext);
	if (x64GenerationSuccess == false)
	{
		return nullptr;
	}
#elif defined(__aarch64__)
	bool aarch64GenerationSuccess = PPCRecompiler_generateAArch64Code(ppcRecFunc, &ppcImlGenContext);
	if (aarch64GenerationSuccess == false)
	{
		return nullptr;
	}
#endif
	if (ActiveSettings::DumpRecompilerFunctionsEnabled())
	{
		FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}.bin", ppcRecFunc->ppcAddress)));
		if (fs)
		{
			fs->writeData(ppcRecFunc->x86Code, ppcRecFunc->x86Size);
			delete fs;
		}
	}

	// collect list of PPC-->x64 entry points
	entryPointsOut.clear();
	for(IMLSegment* imlSegment : ppcImlGenContext.segmentList2)
	{
		if (imlSegment->isEnterable == false)
			continue;

		uint32 ppcEnterOffset = imlSegment->enterPPCAddress;
		uint32 x64Offset = imlSegment->x64Offset;

		entryPointsOut.emplace_back(ppcEnterOffset, x64Offset);
	}

#if PPCREC_LOG_RECOMPILATION_RESULTS
	bt.Stop();
	uint32 codeHash = 0;
	for (uint32 i = 0; i < ppcRecFunc->x86Size; i++)
	{
		codeHash = _rotr(codeHash, 3);
		codeHash += ((uint8*)ppcRecFunc->x86Code)[i];
	}
	cemuLog_log(LogType::Force, "[Recompiler] PPC 0x{:08x} -> x64: 0x{:x} Took {:.4}ms | Size {:04x} CodeHash {:08x}", (uint32)ppcRecFunc->ppcAddress, (uint64)(uintptr_t)ppcRecFunc->x86Code, bt.GetElapsedMilliseconds(), ppcRecFunc->x86Size, codeHash);
#endif

	return ppcRecFunc;
}

void PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext_t& ppcImlGenContext)
{
	IMLRegisterAllocatorParameters raParam;

	for (auto& it : ppcImlGenContext.mappedRegs)
		raParam.regIdToName.try_emplace(it.second.GetRegID(), it.first);

#if defined(ARCH_X86_64)
	// Limit successor-only reload handling to executables with confirmed dropped reloads.
	raParam.reloadBranchLiveIns =
		(CafeSystem::GetForegroundTitleId() == 0x0005000010106B00ull &&
			CafeSystem::GetForegroundTitleVersion() == 80 && CafeSystem::GetRPXHashUpdated() == 0x83ce0fe9) ||
		(CafeSystem::GetForegroundTitleId() == 0x0005000010138B00ull &&
			CafeSystem::GetForegroundTitleVersion() == 32 && CafeSystem::GetRPXHashUpdated() == 0x60b0d3a7);
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RAX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBP);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RSI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R8);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R9);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R10);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R11);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R12);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RCX);

	// add XMM registers, except XMM15 which is the temporary register
	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 0);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 1);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 2);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 3);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 4);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 5);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 6);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 7);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 8);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 9);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 10);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 11);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 12);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 13);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 14);
#elif defined(__aarch64__)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	for (auto i = IMLArchAArch64::PHYSREG_GPR_BASE; i < IMLArchAArch64::PHYSREG_GPR_BASE + IMLArchAArch64::PHYSREG_GPR_COUNT; i++)
	{
		if (i == IMLArchAArch64::PHYSREG_GPR_BASE + 18)
			continue; // Skip reserved platform register
		gprPhysPool.SetAvailable(i);
	}

	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	for (auto i = IMLArchAArch64::PHYSREG_FPR_BASE; i < IMLArchAArch64::PHYSREG_FPR_BASE + IMLArchAArch64::PHYSREG_FPR_COUNT; i++)
		fprPhysPool.SetAvailable(i);
#endif

	IMLRegisterAllocator_AllocateRegisters(&ppcImlGenContext, raParam);
}

bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext)
{
	// isolate entry points from function flow (enterable segments must not be the target of any other segment)
	// this simplifies logic during register allocation
	PPCRecompilerIML_isolateEnterableSegments(&ppcImlGenContext);

	// merge certain float load+store patterns
	IMLOptimizer_OptimizeDirectFloatCopies(&ppcImlGenContext);
	// delay byte swapping for certain load+store patterns
	IMLOptimizer_OptimizeDirectIntegerCopies(&ppcImlGenContext);

	IMLOptimizer_StandardOptimizationPass(ppcImlGenContext);

	PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext);

	return true;
}

bool PPCRecompiler_makeRecompiledFunctionActive(uint32 initialEntryPoint, PPCFunctionBoundaryTracker::PPCRange_t& range, PPCRecFunction_t* ppcRecFunc, std::vector<std::pair<MPTR, uint32>>& entryPoints)
{
	// update jump table
	s_ppcRecompilerState.recompilerSpinlock.lock();

	// check if the initial entrypoint is still flagged for recompilation
	// its possible that the range has been invalidated during the time it took to translate the function
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[initialEntryPoint / 4] != PPCRecompiler_leaveRecompilerCode_visited)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return false;
	}

	// check if the current range got invalidated during the time it took to recompile it
	bool isInvalidated = false;
	for (auto& invRange : s_ppcRecompilerState.invalidationRanges)
	{
		MPTR rStartAddr = invRange.startAddress;
		MPTR rEndAddr = rStartAddr + invRange.size;
		for (auto& recFuncRange : ppcRecFunc->list_ranges)
		{
			if (recFuncRange.ppcAddress < (rEndAddr) && (recFuncRange.ppcAddress + recFuncRange.ppcSize) > rStartAddr)
			{
				isInvalidated = true;
				break;
			}
		}
	}
	s_ppcRecompilerState.invalidationRanges.clear();
	if (isInvalidated)
	{
		s_ppcRecompilerState.recompilerSpinlock.unlock();
		return false;
	}

	// update jump table and remember which entries we updated
	cemu_assert_debug(ppcRecFunc->jumpTableEntries.empty());
	for (auto& itr : entryPoints)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[itr.first / 4] = (PPCREC_JUMP_ENTRY)((uint8*)ppcRecFunc->x86Code + itr.second);
		ppcRecFunc->jumpTableEntries.emplace_back(itr.first, ((uint8*)ppcRecFunc->x86Code + itr.second));
	}

	// due to inlining, some entrypoints can get optimized away
	// therefore we reset all addresses that are still marked as visited (but not recompiled)
	// we dont remove the points from the queue but any address thats not marked as visited won't get recompiled
	// if they are reachable, the interpreter will queue them again
	for (uint32 v = range.startAddress; v < (range.startAddress + range.length); v += 4)
	{
		auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4];
		if (funcPtr == PPCRecompiler_leaveRecompilerCode_visited)
			ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}

	// register ranges
	for (auto& r : ppcRecFunc->list_ranges)
	{
		r.storedRange = s_ppcRecompilerState.functionStorage.storeRange(ppcRecFunc, r.ppcAddress, r.ppcAddress + r.ppcSize);
	}
	s_ppcRecompilerState.recompilerSpinlock.unlock();
	return true;
}

void PPCRecompiler_recompileAtAddress(uint32 address)
{
	cemu_assert_debug(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[address / 4] == PPCRecompiler_leaveRecompilerCode_visited);

	// get size
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(address);
	// get range that encompasses address
	PPCFunctionBoundaryTracker::PPCRange_t range;
	if (funcBoundaries.getRangeForAddress(address, range) == false)
	{
		cemu_assert_debug(false);
	}

	// todo - use info from previously compiled ranges to determine full size of this function (and merge all the entryAddresses)

	// collect all currently known entry points for this range
	s_ppcRecompilerState.recompilerSpinlock.lock();
	std::set<uint32> entryAddresses;
	entryAddresses.emplace(address);
	s_ppcRecompilerState.recompilerSpinlock.unlock();

	std::vector<std::pair<MPTR, uint32>> functionEntryPoints;
	PPCRecFunction_t* func = PPCRecompiler_recompileFunction(range, entryAddresses, functionEntryPoints, funcBoundaries);
	if (!func)
		return; // recompilation failed
	PPCRecompiler_makeRecompiledFunctionActive(address, range, func, functionEntryPoints);
}

void PPCRecompiler_thread()
{
	SetThreadName("PPCRecompiler");
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	return;
#endif

	while (true)
	{
        if(s_ppcRecompilerState.workerThreadStopSignal)
            return;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		// asynchronous recompilation:
		// 1) take address from queue
		// 2) check if address is still marked as visited
		// 3) if yes -> calculate size, gather all entry points, recompile and update jump table
		while (true)
		{
			s_ppcRecompilerState.recompilerSpinlock.lock();
			if (s_ppcRecompilerState.targetQueue.empty() && s_ppcRecompilerState.ac4DependencyQueue.empty())
			{
				s_ppcRecompilerState.recompilerSpinlock.unlock();
				break;
			}
			const bool dependency = !s_ppcRecompilerState.ac4DependencyQueue.empty();
			auto& queue = dependency ? s_ppcRecompilerState.ac4DependencyQueue : s_ppcRecompilerState.targetQueue;
			auto enterAddress = queue.front();
			queue.pop();

			auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
			if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
			{
				// only recompile functions if marked as visited
				s_ppcRecompilerState.recompilerSpinlock.unlock();
				continue;
			}
			s_ppcRecompilerState.recompilerSpinlock.unlock();

			PPCRecompiler_recompileAtAddress(enterAddress);
			if(s_ppcRecompilerState.workerThreadStopSignal)
				return;
		}
	}
}

#define PPC_REC_ALLOC_BLOCK_SIZE	(4*1024*1024) // 4MB

constexpr uint32 PPCRecompiler_GetNumAddressSpaceBlocks()
{
    return (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;
}

std::bitset<PPCRecompiler_GetNumAddressSpaceBlocks()> ppcRecompiler_reservedBlockMask;

void PPCRecompiler_reserveLookupTableBlock(uint32 offset)
{
	uint32 blockIndex = offset / PPC_REC_ALLOC_BLOCK_SIZE;
	offset = blockIndex * PPC_REC_ALLOC_BLOCK_SIZE;

	if (ppcRecompiler_reservedBlockMask[blockIndex])
		return;
	ppcRecompiler_reservedBlockMask[blockIndex] = true;

	void* p = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	if( !p )
	{
		cemuLog_log(LogType::Force, "Failed to allocate memory for recompiler (0x{:08x})", offset);
		cemu_assert(false);
		return;
	}
	for(uint32 i=0; i<PPC_REC_ALLOC_BLOCK_SIZE/4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4+i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	uint32 endAddress = (startAddress + size + PPC_REC_ALLOC_BLOCK_SIZE - 1) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = (startAddress) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = std::min(startAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	endAddress = std::min(endAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	for (uint32 i = startAddress; i < endAddress; i += PPC_REC_ALLOC_BLOCK_SIZE)
	{
		PPCRecompiler_reserveLookupTableBlock(i);
	}
}

bool PPCRecompiler_findFuncRanges(uint32 addr, ppcRecompilerFuncRange* rangesOut, size_t* countInOut)
{
	s_ppcRecompilerState.recompilerSpinlock.lock();
	size_t countIn = *countInOut;
	size_t countOut = 0;

	s_ppcRecompilerState.functionStorage.findRanges(addr, addr + 4, [rangesOut, countIn, &countOut](uint32 start, uint32 end, PPCRecFunction_t* func)
	{
		if (countOut < countIn)
		{
			rangesOut[countOut].ppcStart = start;
			rangesOut[countOut].ppcSize = (end-start);
			rangesOut[countOut].x86Start = func->x86Code;
			rangesOut[countOut].x86Size = func->x86Size;
		}
		countOut++;
	}
	);
	s_ppcRecompilerState.recompilerSpinlock.unlock();
	*countInOut = countOut;
	if (countOut > countIn)
		return false;
	return true;
}

extern "C" DLLEXPORT uintptr_t* PPCRecompiler_getJumpTableBase()
{
	if (ppcRecompilerInstanceData == nullptr)
		return nullptr;
	return (uintptr_t*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
}

void PPCRecompiler_deleteFunction(PPCRecFunction_t* func)
{
	cemu_assert_debug(s_ppcRecompilerState.recompilerSpinlock.is_locked());
	// unlink entrypoints from JumpTable
	for (auto& entrypoint : func->jumpTableEntries)
	{
		if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[entrypoint.ppcAddr / 4] == entrypoint.hostEntrypoint)
			ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[entrypoint.ppcAddr / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
	// delete from storage
	for (auto& r : func->list_ranges)
	{
		if(r.storedRange)
			s_ppcRecompilerState.functionStorage.deleteRange(r.storedRange);
		r.storedRange = nullptr;
	}
	// todo - free x86 code
}

void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr)
{
	if (!s_ppcRecompilerState.initialized)
		return;
	if (startAddr >= PPC_REC_CODE_AREA_SIZE)
		return;
	cemu_assert_debug(endAddr >= startAddr);

	s_ppcRecompilerState.recompilerSpinlock.lock();

	uint32 rStart;
	uint32 rEnd;
	PPCRecFunction_t* rFunc;
	// delete functions which intersect the invalidated range
	while (s_ppcRecompilerState.functionStorage.findFirstRange(startAddr, endAddr, rStart, rEnd, rFunc))
		PPCRecompiler_deleteFunction(rFunc);
	// add entry to invalidation queue, this is used to invalidate functions for which recompilation has already started
	s_ppcRecompilerState.invalidationRanges.emplace_back(startAddr, endAddr-startAddr);

	s_ppcRecompilerState.recompilerSpinlock.unlock();
}

#if defined(ARCH_X86_64)
void PPCRecompiler_initPlatform()
{
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[1] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[0] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[1] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[1] = ~0ULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[1] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[0] = ~(1 << 31);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[1] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[2] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[3] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_singleWordMask[0] = 0xFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_singleWordMask[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[0] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[1] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[0] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[1] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[0] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[1] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[0] = 1.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[1] = 1.0f;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[0] = 0x00800000;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[1] = 0x00800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[0] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[1] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[2] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[3] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[0] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[1] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[2] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[3] = ~0x80000000;

	// mxcsr
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOn = 0x1F80 | 0x8000;
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOff = 0x1F80;
}
#else
void PPCRecompiler_initPlatform()
{

}
#endif

void PPCRecompiler_init()
{
	s_ppcRecompilerState.ac4DependencyQueue = {};
	s_ppcRecompilerState.ac4PromotedDependencies.clear();
	s_ppcRecompilerState.recompilerEnableCount = 0;
	if (ActiveSettings::GetCPUMode() == CPUMode::SinglecoreInterpreter)
	{
		cemuLog_log(LogType::Force, "Using singlecore interpreter");
		return;
	}
	if (LaunchSettings::ForceInterpreter() || LaunchSettings::ForceMultiCoreInterpreter())
	{
		cemuLog_log(LogType::Force, "Recompiler disabled. Command line --force-interpreter or force-multicore-interpreter was passed");
		return;
	}
	if (ppcRecompilerInstanceData)
	{
		MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
		ppcRecompilerInstanceData = nullptr;
	}
	cemuLog_logDebug(LogType::Force, "Reserving {}MB for recompiler instance data", (sint32)(sizeof(PPCRecompilerInstanceData_t) / 1024 / 1024));
	ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
	MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true);
#ifdef ARCH_X86_64
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
#elif defined(__aarch64__)
	PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
#endif
    PPCRecompiler_allocateRange(0, 0x1000); // the first entry is used for fallback to interpreter
    PPCRecompiler_allocateRange(mmuRange_TRAMPOLINE_AREA.getBase(), mmuRange_TRAMPOLINE_AREA.getSize());
    PPCRecompiler_allocateRange(mmuRange_CODECAVE.getBase(), mmuRange_CODECAVE.getSize());

    PPCRecompiler_initPlatform();

	cemuLog_log(LogType::Force, "Recompiler initialized");

	s_ppcRecompilerState.initialized = true;
	s_ppcRecompilerState.recompilerEnableCount = 1; // enabled

	// launch recompilation thread
    s_ppcRecompilerState.workerThreadStopSignal = false;
    s_ppcRecompilerState.workerThread = std::thread(PPCRecompiler_thread);
}

void PPCRecompiler_Shutdown()
{
    // shut down recompiler thread
    s_ppcRecompilerState.workerThreadStopSignal = true;
    if(s_ppcRecompilerState.workerThread.joinable())
        s_ppcRecompilerState.workerThread.join();
    // clean up queues
    while(!s_ppcRecompilerState.targetQueue.empty())
        s_ppcRecompilerState.targetQueue.pop();
	s_ppcRecompilerState.ac4DependencyQueue = {};
	s_ppcRecompilerState.ac4PromotedDependencies.clear();
    s_ppcRecompilerState.invalidationRanges.clear();
    // clean range store
    s_ppcRecompilerState.functionStorage.clear();
    // clean up memory
    uint32 numBlocks = PPCRecompiler_GetNumAddressSpaceBlocks();
    for(uint32 i=0; i<numBlocks; i++)
    {
        if(!ppcRecompiler_reservedBlockMask[i])
            continue;
        // deallocate
        uint64 offset = i * PPC_REC_ALLOC_BLOCK_SIZE;
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        // mark as unmapped
        ppcRecompiler_reservedBlockMask[i] = false;
    }
	s_ppcRecompilerState.recompilerEnableCount = 0;
	s_ppcRecompilerState.initialized = false;
}

// For each Enable call, Disable needs to be called once and vice versa
void PPCRecompiler_Enable()
{
	if (s_ppcRecompilerState.initialized)
		s_ppcRecompilerState.recompilerEnableCount++;
}

void PPCRecompiler_Disable()
{
	if (s_ppcRecompilerState.initialized)
		s_ppcRecompilerState.recompilerEnableCount--;
}
