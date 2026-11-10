#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Memory.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/Nba2K13Compatibility.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/OS/libs/gx2/GX2_Surface.h"

namespace coreinit
{

	void DCInvalidateRange(MPTR addr, uint32 size)
	{
		MPTR addrEnd = (addr + size + 0x1F) & ~0x1F;
		addr &= ~0x1F;
		//LatteBufferCache_notifyDCFlush(addr, addrEnd - addr);
	}

	void DCFlushRange(MPTR addr, uint32 size)
	{
		MPTR addrEnd = (addr + size + 0x1F) & ~0x1F;
		addr &= ~0x1F;
		LatteBufferCache_notifyDCFlush(addr, addrEnd - addr);
	}

	void DCFlushRangeNoSync(MPTR addr, uint32 size)
	{
		MPTR addrEnd = (addr + size + 0x1F) & ~0x1F;
		addr &= ~0x1F;
		LatteBufferCache_notifyDCFlush(addr, addrEnd - addr);
	}

	void DCStoreRange(MPTR addr, uint32 size)
	{
		MPTR addrEnd = (addr + size + 0x1F) & ~0x1F;
		addr &= ~0x1F;
		// NBA publishes rewritten vertex data with the synchronized store call.
		// Its buffer cache must observe the same writeback as the NoSync variant.
		if (size && addrEnd > addr && Nba2K13Compatibility::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()))
			LatteBufferCache_notifyDCFlush(addr, addrEnd - addr);
	}

	void DCStoreRangeNoSync(MPTR addr, uint32 size)
	{
		MPTR addrEnd = (addr + size + 0x1F) & ~0x1F;
		addr &= ~0x1F;
		LatteBufferCache_notifyDCFlush(addr, addrEnd - addr);
	}

	void DCZeroRange(MPTR addr, uint32 size)
	{
		MPTR alignedAddr = addr & ~31;
		uint32 cachlineOffset = addr & 31;
		uint32 blocks = (cachlineOffset + size + 31) / 32;

		if (blocks > 0)
		{
			memset(memory_getPointerFromVirtualOffset(alignedAddr), 0x00, blocks * 32);
			LatteBufferCache_notifyDCFlush(alignedAddr, blocks * 32);
		}
	}

	bool OSIsAddressRangeDCValid(uint32 startOffset, uint32 range)
	{
		uint32 endOffset = startOffset + range - 1;
		uint32 boundaryLow = 0xE8000000;
		uint32 boundaryHigh = 0xEC000000;
		if (startOffset < boundaryLow || startOffset >= boundaryHigh)
			return false;
		if (endOffset < boundaryLow || endOffset >= boundaryHigh)
			return false;
		return true;
	}

	void* coreinit_memset(void* dst, uint32 value, uint32 size)
	{
		memset(dst, value, size);
		return dst;
	}

	void* coreinit_memcpy(MEMPTR<void> dst, MEMPTR<void> src, uint32 size)
	{
		if (dst.GetMPTR() == 0xFFFFFFFF)
		{
			// games this was seen in: The Swapper
			// this may be a bug in the game. The last few bytes of the address space are writable, but wrap-around behavior of COS memcpy is unknown
			cemu_assert_debug(false);
		}
		if (size > 0)
		{
			if (size == 320 * 320 * 4 && CafeSystem::GetForegroundTitleId() == 0x0005000010144f00ull &&
				CafeSystem::GetForegroundTitleVersion() == 304 && CafeSystem::GetRPXHashUpdated() == 0xc80dd694)
			{
				// Portrait pixels can be copied directly, without GX2CopySurface.
				// The renderer checks for a GPU-written source before touching guest RAM.
				LatteAsyncCommands_queueSmashPortraitReadback(memory_virtualToPhysical(src.GetMPTR()));
				LatteAsyncCommands_waitUntilAllProcessed();
			}
			if (size == 128 * 128 * 4 && CafeSystem::GetForegroundTitleId() == 0x0005000010128f00ull &&
				CafeSystem::GetForegroundTitleVersion() == 48 && CafeSystem::GetRPXHashUpdated() == 0x78278a63)
			{
				auto* cpu = PPCInterpreter_getCurrentInstance();
				if (cpu && cpu->spr.LR == 0x021dc040)
				{
					// createD3DTexture keeps its completed FFL icon surface in r31.
					// Publish GPU pixels before the game's CPU copy reads the backing RAM.
					auto* surface = (GX2Surface*)memory_getPointerFromVirtualOffset(cpu->gpr[31]);
					if (surface->imagePtr == src.GetMPTR() && surface->imageSize == size &&
						surface->width == 128 && surface->height == 128 && surface->depth == 1 &&
						surface->numLevels == 1 && surface->aa == 0 && (surface->swizzle & 0x700) == 0 &&
						surface->format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM &&
						surface->tileMode == Latte::E_GX2TILEMODE::TM_2D_TILED_THIN1)
					{
						LatteAsyncCommands_queueForceTextureReadback(memory_virtualToPhysical(surface->imagePtr),
							0, 0, (sint32)surface->format.value(), 128, 128, 1, surface->pitch, 0,
							(sint32)surface->dim.value(), Latte::E_HWTILEMODE::TM_2D_TILED_THIN1, 0, 0);
						LatteAsyncCommands_waitUntilAllProcessed();
					}
				}
			}
			memcpy(dst.GetPtr(), src.GetPtr(), size);
			// always flushes the cache!
			LatteBufferCache_notifyDCFlush(dst.GetMPTR(), size);
		}

		return dst.GetPtr();
	}

	void* coreinit_memmove(MEMPTR<void> dst, void* src, uint32 size)
	{
		if (size > 0)
		{
			memmove(dst.GetPtr(), src, size);
			// always flushes the cache!
			LatteBufferCache_notifyDCFlush(dst.GetMPTR(), size);
		}
		return dst.GetPtr();
	}

	void* OSBlockMove(MEMPTR<void> dst, MEMPTR<void> src, uint32 size, bool flushDC)
	{
		if (size > 0)
		{
			memmove(dst.GetPtr(), src.GetPtr(), size);
			if (flushDC)
				LatteBufferCache_notifyDCFlush(dst.GetMPTR(), size);
		}
		return dst.GetPtr();
	}

	void* OSBlockSet(MEMPTR<void> dst, uint32 value, uint32 size)
	{
		memset(dst.GetPtr(), value&0xFF, size);
		return dst.GetPtr();
	}

	MPTR OSEffectiveToPhysical(MPTR effectiveAddr)
	{
		MPTR physicalAddr = memory_virtualToPhysical(effectiveAddr);
		return physicalAddr;
	}

	void OSMemoryBarrier()
	{
		// no-op
	}

	void OSGetMemBound(sint32 memType, MEMPTR<void>* offsetOutput, uint32be* sizeOutput)
	{
		MPTR memAddr = MPTR_NULL;
		uint32 memSize = 0;

		/*
 		Data taken from browser dump:
		type	start		size
		MEM1	0xF4000000	0x02000000
		MEM2	0x106DE000	0x170C2000
		*/
		if (memType == 1)
		{
			// MEM1
			memAddr = mmuRange_MEM1.getBase();
			memSize = mmuRange_MEM1.getSize();
		}
		else if (memType == 2)
		{
			// MEM2
			uint32 currentRPLAllocatorOffset = RPLLoader_GetDataAllocatorAddr();

			// due to differences in our library implementations we currently allocate less memory for the OS/RPLs than on the actual hardware,
			// as a result more memory is available to games
			// however, some games crash due to internal overflows if there is too much memory available

			// here we artificially reduce the available memory for the affected games
			uint64 titleId = CafeSystem::GetForegroundTitleId();
			if (
				titleId == 0x0005000010132400ULL || // Lego Marvel Super Heroes (EU)
				titleId == 0x0005000010132B00ULL || // Lego Marvel Super Heroes (US)
				titleId == 0x0005000010194200ull || // Lego Dimensions (US)
				titleId == 0x0005000010195D00ull || // Lego Dimensions (EU)
				titleId == 0x00050000101A6200ull || // Lego Jurassic World (US)
				titleId == 0x00050000101A5C00 || // Lego Jurassic World (EU)
				titleId == 0x000500001014DE00 || // The Lego Movie Videogame (US)
				titleId == 0x000500001014E000 || // The Lego Movie Videogame (EU)
				titleId == 0x0005000010168D00 || // Lego The Hobbit (EU)
				titleId == 0x000500001016A700 || // Lego The Hobbit (JP)
				// The Hobbit US title id?
				titleId == 0x00050000101DAB00 || // Lego Star Wars: The Force Awakens  (US)
				titleId == 0x00050000101DAA00 ||  // Lego Star Wars: The Force Awakens  (EU)
				// LEGO Batman 3: BEYOND GOTHAM 
				titleId == 0x000500001016A400 || // EU
				titleId == 0x000500001016AD00 || // US
				// Lego Marvel Avengers
				titleId == 0x00050000101BE900 || // EU
				titleId == 0x00050000101BEF00 || // US
				// LEGO BATMAN 2: DC Super Heroes 
				titleId == 0x0005000010135500 || // EU
				titleId == 0x0005000010135E00 // US
				)
			{
				cemuLog_logDebug(LogType::Force, "Hack: Reduce available memory to simulate loaded RPLs");
				currentRPLAllocatorOffset += (48 * 1024 * 1024); // 48MB
			}
			memAddr = currentRPLAllocatorOffset;
			memSize = mmuRange_MEM2.getEnd() - currentRPLAllocatorOffset;
		}
		else
		{
			cemu_assert_debug(false);
		}
		if (offsetOutput)
			*offsetOutput = memAddr;
		if (sizeOutput)
			*sizeOutput = memSize;
	}

	void InitializeMemory()
	{
		cafeExportRegister("coreinit", DCInvalidateRange, LogType::Placeholder);
		cafeExportRegister("coreinit", DCFlushRange, LogType::Placeholder);
		cafeExportRegister("coreinit", DCFlushRangeNoSync, LogType::Placeholder);
		cafeExportRegister("coreinit", DCStoreRange, LogType::Placeholder);
		cafeExportRegister("coreinit", DCStoreRangeNoSync, LogType::Placeholder);
		cafeExportRegister("coreinit", DCZeroRange, LogType::Placeholder);
		cafeExportRegister("coreinit", OSIsAddressRangeDCValid, LogType::Placeholder);

		cafeExportRegisterFunc(coreinit_memcpy, "coreinit", "memcpy", LogType::Placeholder);
		cafeExportRegisterFunc(coreinit_memset, "coreinit", "memset", LogType::Placeholder);
		cafeExportRegisterFunc(coreinit_memmove, "coreinit", "memmove", LogType::Placeholder);
		cafeExportRegister("coreinit", OSBlockMove, LogType::Placeholder);
		cafeExportRegister("coreinit", OSBlockSet, LogType::Placeholder);

		cafeExportRegister("coreinit", OSEffectiveToPhysical, LogType::Placeholder);
		cafeExportRegister("coreinit", OSMemoryBarrier, LogType::Placeholder);

		cafeExportRegister("coreinit", OSGetMemBound, LogType::Placeholder);
	}

}
