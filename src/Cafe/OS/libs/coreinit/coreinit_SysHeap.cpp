#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_SysHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"

namespace coreinit
{
	coreinit::MEMHeapHandle _sysHeapHandle = MPTR_NULL;
	sint32 _sysHeapAllocCounter = 0;
	sint32 _sysHeapFreeCounter = 0;

	void* OSAllocFromSystem(uint32 size, uint32 alignment)
	{
		_sysHeapAllocCounter++;
		return coreinit::MEMAllocFromExpHeapEx(_sysHeapHandle, size, alignment);
	}

	void OSFreeToSystem(void* ptr)
	{
		_sysHeapFreeCounter++;
		coreinit::MEMFreeToExpHeap(_sysHeapHandle, ptr);
	}

	void InitSysHeap()
	{
		uint32 sysHeapSize = 8 * 1024 * 1024; // actual size is unknown
		// Recreate title heap contents without leaking another 8 MB on every launch.
		// Other system-area allocations can be cached for the entire host process.
		static MPTR sysHeapStorage = MPTR_NULL;
		if (sysHeapStorage == MPTR_NULL)
			sysHeapStorage = coreinit_allocFromSysArea(sysHeapSize, 0x1000);
		MEMPTR<void> heapBaseAddress = memory_getPointerFromVirtualOffset(sysHeapStorage);
		_sysHeapHandle = coreinit::MEMCreateExpHeapEx(heapBaseAddress.GetPtr(), sysHeapSize, MEM_HEAP_OPTION_THREADSAFE);
		_sysHeapAllocCounter = 0;
		_sysHeapFreeCounter = 0;
	}

	void InitializeSysHeap()
	{
		cafeExportRegister("h264", OSAllocFromSystem, LogType::CoreinitMem);
		cafeExportRegister("h264", OSFreeToSystem, LogType::CoreinitMem);
	}

}
