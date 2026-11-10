#pragma once

#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "config/ActiveSettings.h"

inline bool UseMadden13AtlasReadback(uint32 width, uint32 height, Latte::E_GX2SURFFMT format)
{
	// CPU compression reads the rendered field and both sideline uniform atlases.
	const bool atlasSize = (width == 1024 && (height == 2048 || height == 1024)) ||
		(width == 512 && height == 512);
	return CafeSystem::GetForegroundTitleId() == 0x000500001010d800ull &&
		CafeSystem::GetForegroundTitleVersion() == 0 && CafeSystem::GetRPXHashUpdated() == 0xde24f7d5 &&
		atlasSize && format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM &&
		(ActiveSettings::GetGraphicsAPI() == kOpenGL || ActiveSettings::GetGraphicsAPI() == kVulkan);
}
