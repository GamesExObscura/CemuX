#pragma once
#include <cstdint>

namespace WatchDogsComposite
{
	constexpr bool NeedsRectangleCullFix(uint64_t title, uint32_t version, uint32_t rpx,
		bool rectangle, bool cullFront, bool cullBack, bool clockwiseFront)
	{
		return title == 0x0005000010142000ull && version == 0 && rpx == 0x95fd128b &&
			rectangle && !cullFront && cullBack && clockwiseFront;
	}
}
