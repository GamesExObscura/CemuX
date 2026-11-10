#pragma once

#include <cstdint>

namespace coreinit::GuestHeapAlignment
{
	// Host reservations need not satisfy the guest's requested alignment.
	constexpr uintptr_t Up(uintptr_t host, uintptr_t base, uintptr_t alignment)
	{
		return base + ((host - base + alignment - 1) & ~(alignment - 1));
	}

	constexpr uintptr_t Down(uintptr_t host, uintptr_t base, uintptr_t alignment)
	{
		return base + ((host - base) & ~(alignment - 1));
	}
}
