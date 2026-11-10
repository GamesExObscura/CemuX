#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/Core/BoomGamePadCopy.h"
#include "Cafe/HW/Latte/Core/BlacklistSurfaceCopy.h"

#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"

namespace
{
	bool UseLegoRamCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteTexture* source)
	{
		const bool target = (CafeSystem::GetForegroundTitleId() == 0x0005000010135e00ull &&
			CafeSystem::GetForegroundTitleVersion() == 0 && CafeSystem::GetRPXHashUpdated() == 0xa5c88d06) ||
			(CafeSystem::GetForegroundTitleId() == 0x000500001016ad00ull &&
			CafeSystem::GetForegroundTitleVersion() == 48 && CafeSystem::GetRPXHashUpdated() == 0x95bc464a) ||
			(CafeSystem::GetForegroundTitleId() == 0x0005000010194200ull &&
			CafeSystem::GetForegroundTitleVersion() == 320 && CafeSystem::GetRPXHashUpdated() == 0x84227143) ||
			(CafeSystem::GetForegroundTitleId() == 0x00050000101a6200ull &&
			CafeSystem::GetForegroundTitleVersion() == 16 && CafeSystem::GetRPXHashUpdated() == 0xcb709aa2) ||
			(CafeSystem::GetForegroundTitleId() == 0x00050000101bef00ull &&
			CafeSystem::GetForegroundTitleVersion() == 64 && CafeSystem::GetRPXHashUpdated() == 0xea5a1561) ||
			(CafeSystem::GetForegroundTitleId() == 0x0005000010132b00ull &&
			CafeSystem::GetForegroundTitleVersion() == 32 && CafeSystem::GetRPXHashUpdated() == 0x9454153b) ||
			(CafeSystem::GetForegroundTitleId() == 0x00050000101dab00ull &&
			CafeSystem::GetForegroundTitleVersion() == 32 && CafeSystem::GetRPXHashUpdated() == 0xd84e4371) ||
			(CafeSystem::GetForegroundTitleId() == 0x000500001016a700ull &&
			CafeSystem::GetForegroundTitleVersion() == 0 && CafeSystem::GetRPXHashUpdated() == 0xfa868d52) ||
			(CafeSystem::GetForegroundTitleId() == 0x000500001014de00ull &&
			CafeSystem::GetForegroundTitleVersion() == 16 && CafeSystem::GetRPXHashUpdated() == 0x821f07a3);
		const bool riseBlocks = CafeSystem::GetForegroundTitleId() == 0x000500001014c100ull &&
			CafeSystem::GetForegroundTitleVersion() == 0 && CafeSystem::GetRPXHashUpdated() == 0xfab814f1 &&
			(src.surfaceFormat == Latte::E_GX2SURFFMT::R32_G32_UINT ||
			 src.surfaceFormat == Latte::E_GX2SURFFMT::R32_G32_B32_A32_UINT);
		return ((target && src.surfaceFormat == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM) || riseBlocks) &&
			dst.surfaceFormat == src.surfaceFormat &&
			src.dim == Latte::E_DIM::DIM_2D && dst.dim == Latte::E_DIM::DIM_2D &&
			src.aa == 0 && dst.aa == 0 && src.sliceIndex == 0 && dst.sliceIndex == 0 &&
			!source->isUpdatedOnGPU && !source->reloadFromDynamicTextures;
	}

	sint32 GetCopyDestinationDepth(const LatteSurfaceCopyParam& dst)
	{
		if (ActiveSettings::UseInjusticeLoopConstants() && dst.sliceIndex >= 0 && dst.sliceIndex < 2048)
		{
			// Copies can populate a later layer before any texture view creates the array.
			if (dst.dim == Latte::E_DIM::DIM_2D_ARRAY)
				return dst.sliceIndex + 1;
			if (dst.dim == Latte::E_DIM::DIM_CUBEMAP)
				return (dst.sliceIndex / 6 + 1) * 6;
		}
		return 1;
	}

	bool UseWalkingDeadDrcDrawCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
	{
		return g_renderer->GetType() == RendererAPI::Vulkan &&
			CafeSystem::GetForegroundTitleId() == 0x000500001011F300ULL &&
			CafeSystem::GetForegroundTitleVersion() == 0 &&
			src.physDataAddr == 0xF4651800 && dst.physDataAddr == 0x18ADC000 &&
			src.surfaceFormat == Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB &&
			dst.surfaceFormat == Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB &&
			src.pitch == 0x420 && dst.pitch == 0x420 &&
			src.tilemode == Latte::E_GX2TILEMODE::TM_2D_TILED_THIN1 &&
			dst.tilemode == Latte::E_GX2TILEMODE::TM_2D_TILED_THIN1 &&
			src.sliceIndex == 0 && dst.sliceIndex == 0 &&
			rect.x == 0 && rect.y == 0 && rect.width == 1056 && rect.height == 594;
	}
}

/* Surface copies are tricky to handle because we simulate unified memory on top of two separate memory systems: RAM and VRAM
 * Cemu may only have texture data in either RAM or VRAM, or both. And doing transfers to or from CPU can quickly become prohibitively expensive
 * So we need to make best guesses on where the data needs to come from and where it needs to go
 * A complicating factor is that texture copies are more like memcpy, in the sense that they don't care about the actual underlying pixel format
 * For example a R32F texture can be copied as RGBA8. Similiarly, different tile modes can sometimes be identical under specific circumstances
 */

void gx2SurfaceCopySoftware(
	uint8* inputData, sint32 surfSrcHeight, sint32 srcPitch, sint32 srcDepth, uint32 srcSlice, uint32 srcSwizzle, uint32 srcHwTileMode,
	uint8* outputData, sint32 surfDstHeight, sint32 dstPitch, sint32 dstDepth, uint32 dstSlice, uint32 dstSwizzle, uint32 dstHwTileMode,
	uint32 copyWidth, uint32 copyHeight, uint32 copyBpp);

void LatteSurfaceCopy_CopyInRAM(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	Latte::E_HWSURFFMT dstHwFormat = Latte::GetHWFormat(dst.surfaceFormat);

	sint32 copyWidth = rect.width;
	sint32 copyHeight = rect.height;
	if (Latte::IsCompressedFormat(dstHwFormat))
	{
		copyWidth = (copyWidth + 3) / 4;
		copyHeight = (copyHeight + 3) / 4;
	}

	uint32 dstBpp = Latte::GetFormatBits(dstHwFormat);

	gx2SurfaceCopySoftware((uint8*)MEMPTR<void>(src.physDataAddr).GetPtr(), src.heightInTexels, src.pitch, 1, src.sliceIndex, src.swizzle, (uint32)src.tilemode,
		(uint8*)MEMPTR<void>(dst.physDataAddr).GetPtr(), dst.heightInTexels, dst.pitch, 1, dst.sliceIndex, dst.swizzle, (uint32)dst.tilemode,
		copyWidth, copyHeight, dstBpp);
}

void LatteSurfaceCopy_copySurfaceNew(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	cemu_assert_debug(rect.x == 0 && rect.y == 0); // origin offset not yet supported

	cemu_assert_debug((rect.x + rect.width) <= dst.pitch * (Latte::IsCompressedFormat(dst.surfaceFormat)?4:1));
	cemu_assert_debug((rect.x + rect.width) <= src.pitch * (Latte::IsCompressedFormat(src.surfaceFormat)?4:1));

	if (src.tilemode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL || dst.tilemode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL)
	{
		// todo - it's technically possible for a matching linear texture to be in the texture cache already
		//        there is also a case of tiled to linear_special where we should trigger a readback without an actual destination texture
		LatteSurfaceCopy_CopyInRAM(src, dst, rect);
		return;
	}

	// look up source texture
	// todo - for non-zero slices heightInTexels matters (used to calculate the slice size). We should take this into account during lookup
	LatteTexture* sourceTexture = nullptr;
	LatteTextureView* sourceView = LatteTC_GetTextureSliceViewOrTryCreate(src.physDataAddr, MPTR_NULL, src.surfaceFormat, Latte::MakeHWTileMode(src.tilemode), rect.x + rect.width, rect.y + rect.height, 1, src.pitch, src.swizzle, src.sliceIndex, 0);
	if (sourceView == nullptr)
	{
		// source texture doesn't exist (yet) in texture cache
		// operate on RAM instead
		LatteSurfaceCopy_CopyInRAM(src, dst, rect);
		return;
	}
	sourceTexture = sourceView->baseTexture;
	if (UseLegoRamCopy(src, dst, sourceTexture))
	{
		// Raw RGBA copies are later sampled through BC texture views. Preserve the
		// shared bytes when RAM is authoritative instead of leaving the BC view stale.
		LatteSurfaceCopy_CopyInRAM(src, dst, rect);
		return;
	}
	if (sourceTexture->reloadFromDynamicTextures)
	{
		LatteTexture_UpdateCacheFromDynamicTextures(sourceTexture);
		sourceTexture->reloadFromDynamicTextures = false;
	}
	// look up destination texture
	LatteTexture* destinationTexture = nullptr;
	LatteTextureView* destinationView = LatteTextureViewLookupCache::lookupSliceMinSize(dst.physDataAddr, rect.x + rect.width, rect.y + rect.height, dst.pitch, 0, dst.sliceIndex, dst.surfaceFormat);
	const bool preserveCars3Stencil = CafeSystem::GetForegroundTitleId() == 0x0005000010209900ull &&
		CafeSystem::GetForegroundTitleVersion() == 16 && CafeSystem::GetRPXHashUpdated() == 0x09ff79b0 &&
		(g_renderer->GetType() == RendererAPI::OpenGL || g_renderer->GetType() == RendererAPI::Vulkan) &&
		sourceTexture->isDepth && sourceTexture->hasStencil &&
		src.surfaceFormat == Latte::E_GX2SURFFMT::D24_S8_UNORM && dst.surfaceFormat == src.surfaceFormat &&
		rect.width == 1280 && rect.height == 672 && src.pitch == dst.pitch;
	if (preserveCars3Stencil && (!destinationView || !destinationView->baseTexture->isDepth))
	{
		// The game later samples X24_G8_UINT object IDs from this copy. A color R32F
		// surrogate preserves depth only and silently discards those stencil bytes.
		destinationView = LatteTexture_CreateMapping(dst.physDataAddr, MPTR_NULL, rect.width, rect.height, 1, dst.pitch,
			Latte::MakeHWTileMode(dst.tilemode), dst.swizzle, 0, 1, dst.sliceIndex, 1, dst.surfaceFormat, dst.dim,
			Latte::E_DIM::DIM_2D, true);
	}
	// todo - Instead of lookupSliceMinSize lookup the base texture by data range instead and return mip/slice index
	if (destinationView)
		destinationTexture = destinationView->baseTexture;
	// create destination texture if it doesnt exist
	if (!destinationTexture)
	{
		const bool blacklist1DCopy = rect.x == 0 && rect.y == 0 &&
			BlacklistSurfaceCopy::IsOneDimensionalCopy(CafeSystem::GetForegroundTitleId(),
				CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(),
				(uint32)src.dim, (uint32)dst.dim, (uint32)src.surfaceFormat, (uint32)dst.surfaceFormat,
				rect.width, rect.height, src.sliceIndex, dst.sliceIndex, src.aa, dst.aa);
		// A 1D destination cannot be represented by the default 2D copy view.
		const auto viewDim = blacklist1DCopy ? Latte::E_DIM::DIM_1D :
			(Latte::IsMSAA(dst.dim) ? Latte::E_DIM::DIM_2D_MSAA : Latte::E_DIM::DIM_2D);
		if (blacklist1DCopy)
			cemuLog_logOnce(LogType::Force, "Splinter Cell Blacklist: preserving 1D copy destination view");
		destinationView = LatteTexture_CreateMapping(dst.physDataAddr, MPTR_NULL, rect.x + rect.width, rect.y + rect.height, GetCopyDestinationDepth(dst), dst.pitch, Latte::MakeHWTileMode(dst.tilemode), dst.swizzle, 0, 1, dst.sliceIndex, 1, dst.surfaceFormat, dst.dim, viewDim, false);
		destinationTexture = destinationView->baseTexture;
	}
	// copy texture
	if (sourceTexture && destinationTexture)
	{
		// mark source and destination texture as still in use
		LatteTC_MarkTextureStillInUse(destinationTexture);
		LatteTC_MarkTextureStillInUse(sourceTexture);
		sint32 realSrcSlice = src.sliceIndex;
		if (LatteTexture_doesEffectiveRescaleRatioMatch(sourceTexture, sourceView->firstMip, destinationTexture, destinationView->firstMip))
		{
			cemu_assert_debug(rect.x == 0 && rect.y == 0);
			sint32 copyWidth = rect.width;
			sint32 copyHeight = rect.height;
			sint32 effectiveCopyWidth = copyWidth;
			sint32 effectiveCopyHeight = copyHeight;
			LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);
			// copy slice
			const bool boomGamePadCopy = rect.x == 0 && rect.y == 0 &&
				src.dim == Latte::E_DIM::DIM_2D && dst.dim == Latte::E_DIM::DIM_2D &&
				BoomGamePadCopy::Matches(CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(),
					(uint32)src.surfaceFormat, (uint32)dst.surfaceFormat, rect.width, rect.height, src.pitch, dst.pitch,
					(uint32)src.tilemode, (uint32)dst.tilemode, src.aa, dst.aa, src.sliceIndex, dst.sliceIndex);
			// A raw bit copy reinterprets RGBA8 bytes as RGB10 and corrupts the DRC image.
			if (sourceView->baseTexture->isDepth != destinationView->baseTexture->isDepth ||
				UseWalkingDeadDrcDrawCopy(src, dst, rect) || boomGamePadCopy)
				g_renderer->surfaceCopy_copySurfaceWithFormatConversion(sourceTexture, sourceView->firstMip, sourceView->firstSlice, destinationTexture, destinationView->firstMip, destinationView->firstSlice, copyWidth, copyHeight);
			else
				g_renderer->texture_copyImageSubData(sourceTexture, sourceView->firstMip, 0, 0, realSrcSlice, destinationTexture, destinationView->firstMip, 0, 0, destinationView->firstSlice, effectiveCopyWidth, effectiveCopyHeight, 1);
			const uint64 eventCounter = LatteTexture_getNextUpdateEventCounter();
			LatteTexture_MarkDynamicTextureAsChanged(destinationTexture->baseView, destinationView->firstSlice, destinationView->firstMip, eventCounter);
		}
		else
		{
			debug_printf("gx2CP_itHLECopySurface(): Copy texture with non-matching effective size\n");
		}
		LatteTC_ResetTextureChangeTracker(destinationTexture);
		// flag texture as updated
		destinationTexture->lastUpdateEventCounter = LatteTexture_getNextUpdateEventCounter();
		destinationTexture->isUpdatedOnGPU = true; // todo - also track update flag per-slice
	}
	else
		debug_printf("Source or destination texture does not exist\n");
	// if the texture is updated from a tiled to a linear format it's a strong indicator for CPU reads
	// in which case we should sync the texture back to CPU RAM
	const bool sourceIsLinear = sourceTexture->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED || sourceTexture->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL;
	const bool destinationIsLinear = destinationTexture->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED || destinationTexture->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL;
	bool shouldReadback = !sourceIsLinear && destinationIsLinear;
	// special case for Bayonetta 2
	if (destinationTexture->width == 8 && destinationTexture->height == 8 && destinationTexture->tileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THIN1)
	{
		cemuLog_logDebug(LogType::Force, "Texture readback after copy for Bayonetta 2 (phys: 0x{:08x})", destinationTexture->physAddress);
		shouldReadback = true;
	}
	if (shouldReadback)
	{
		LatteTextureReadback_Initate(destinationView);
	}
}
