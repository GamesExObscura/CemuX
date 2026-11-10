#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/CafeSystem.h"
#include "Cemu/Debug/CompatibilityDiagnostics.h"
#include "Cafe/HW/Latte/Core/Nba2K13Compatibility.h"
#include "Cafe/HW/Latte/Core/SwapForcePublishedVideo.h"
#include "Cafe/HW/Latte/Core/PrimePublishedVideo.h"
#include "Cafe/HW/Latte/Core/NbaPublishedVideo.h"
#include "Cafe/HW/Latte/Core/TextureDecoder_Packed422.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"

#ifdef ENABLE_OPENGL
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#endif

struct TexScaleXY
{
	float xy[2];
};

struct
{
	TexScaleXY perUnit[Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE]; // stores actualResolution/effectiveResolution ratio for each texture
}LatteTextureScale[static_cast<size_t>(LatteConst::ShaderType::TotalCount)] = { };

float* LatteTexture_getEffectiveTextureScale(LatteConst::ShaderType shaderType, sint32 texUnit)
{
	cemu_assert_debug(texUnit >= 0 && texUnit < Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
	return LatteTextureScale[static_cast<size_t>(shaderType)].perUnit[texUnit].xy;
}

void LatteTexture_setEffectiveTextureScale(LatteConst::ShaderType shaderType, sint32 texUnit, float u, float v)
{
	cemu_assert_debug(texUnit >= 0 && texUnit < Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
	float* t = LatteTextureScale[static_cast<size_t>(shaderType)].perUnit[texUnit].xy;
	t[0] = u;
	t[1] = v;
}

void LatteTextureLoader_UpdateTextureSliceData(LatteTexture* tex, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr, MPTR physMipPtr, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels, uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle, bool dumpTex);

void LatteTexture_ReloadData(LatteTexture* tex)
{
	tex->reloadCount++;
	for(sint32 mip=tex->hasUnallocatedBaseLevel ? 1 : 0; mip<tex->mipLevels; mip++)
	{
		if(tex->dim == Latte::E_DIM::DIM_2D_ARRAY ||
			tex->dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA )
		{
			sint32 numSlices = std::max(tex->depth, 1);
			for(sint32 s=0; s<numSlices; s++)
				LatteTextureLoader_UpdateTextureSliceData(tex, s, mip, tex->physAddress, tex->physMipAddress, tex->dim, tex->width, tex->height, tex->depth, tex->mipLevels, tex->pitch, tex->tileMode, tex->swizzle, true);
		}
		else if( tex->dim == Latte::E_DIM::DIM_CUBEMAP )
		{
			cemu_assert_debug((tex->depth % 6) == 0);
			sint32 numFullCubeMaps = tex->depth/6; // number of cubemaps (if numFullCubeMaps is >1 then this texture is a cubemap array)
			for(sint32 s=0; s<numFullCubeMaps*6; s++)
				LatteTextureLoader_UpdateTextureSliceData(tex, s, mip, tex->physAddress, tex->physMipAddress, tex->dim, tex->width, tex->height, tex->depth, tex->mipLevels, tex->pitch, tex->tileMode, tex->swizzle, true);
		}
		else if( tex->dim == Latte::E_DIM::DIM_3D )
		{
			sint32 mipDepth = std::max(tex->depth>>mip, 1);
			for(sint32 s=0; s<mipDepth; s++)
			{
				LatteTextureLoader_UpdateTextureSliceData(tex, s, mip, tex->physAddress, tex->physMipAddress, tex->dim, tex->width, tex->height, tex->depth, tex->mipLevels, tex->pitch, tex->tileMode, tex->swizzle, true);
			}
		}
		else
		{
			// load slice 0
			LatteTextureLoader_UpdateTextureSliceData(tex, 0, mip, tex->physAddress, tex->physMipAddress, tex->dim, tex->width, tex->height, tex->depth, tex->mipLevels, tex->pitch, tex->tileMode, tex->swizzle, true);
		}
	}
	tex->lastUpdateEventCounter = LatteTexture_getNextUpdateEventCounter();
}

LatteTextureView* LatteTexture_CreateTexture(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth, bool hasUnallocatedBaseLevel)
{
	const auto tex = g_renderer->texture_createTextureEx(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
	tex->hasUnallocatedBaseLevel = hasUnallocatedBaseLevel;

	// init slice/mip info array
	LatteTexture_InitSliceAndMipInfo(tex);
	LatteTexture_RegisterTextureMemoryOccupancy(tex);
	cemu_assert_debug(mipLevels != 0);

	LatteTexture_ReloadData(tex);
	LatteTC_MarkTextureStillInUse(tex);
	LatteTC_RegisterTexture(tex);

	// create initial view that maps to the whole texture
	tex->baseView = tex->GetOrCreateView(0, tex->mipLevels, 0, tex->depth);
	return tex->baseView;
}

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N& texUnitWord1, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N& texUnitWord4)
{
	Latte::E_GX2SURFFMT gx2Format = (Latte::E_GX2SURFFMT)texUnitWord1.get_DATA_FORMAT();
	auto nfa = texUnitWord4.get_NUM_FORM_ALL();
	if (nfa == Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_SCALED)
		gx2Format |= Latte::E_GX2SURFFMT::FMT_BIT_FLOAT;
	else if (nfa == Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_INT)
		gx2Format |= Latte::E_GX2SURFFMT::FMT_BIT_INT;

	if(texUnitWord4.get_FORCE_DEGAMMA())
		gx2Format |= Latte::E_GX2SURFFMT::FMT_BIT_SRGB;

	if (texUnitWord4.get_FORMAT_COMP_X() == Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP::COMP_SIGNED)
		gx2Format |= Latte::E_GX2SURFFMT::FMT_BIT_SIGNED;

	return gx2Format;
}

static bool IsBatmanResidentMipChain(const LatteDecompilerShader* shader, sint32 unit, const _LatteRegisterSetTextureUnit& resource)
{
	const bool isOrigins = CafeSystem::GetForegroundTitleId() == 0x0005000010137C00ull &&
		CafeSystem::GetForegroundTitleVersion() == 16 && CafeSystem::GetRPXHashUpdated() == 0x07F70478;
	const bool isCity = CafeSystem::GetForegroundTitleId() == 0x000500001010AB00ull &&
		CafeSystem::GetForegroundTitleVersion() == 16 && CafeSystem::GetRPXHashUpdated() == 0xD0330005;
	const auto format = resource.word1.get_DATA_FORMAT();
	if ((!isOrigins && !isCity) ||
		resource.word6.get_TYPE() != Latte::LATTE_SQ_TEX_RESOURCE_WORD6_N::E_TYPE::VTX_VALID_TEXTURE ||
		resource.word3.get_MIP_ADDRESS() == 0 || resource.word0.get_DIM() != Latte::E_DIM::DIM_2D ||
		(format != Latte::E_HWSURFFMT::HWFMT_BC1 &&
		 (!isCity || (format != Latte::E_HWSURFFMT::HWFMT_BC3 && format != Latte::E_HWSURFFMT::HWFMT_8))) ||
		resource.word1.get_DEPTH() != 0 || resource.word5.get_BASE_ARRAY() != 0 || resource.word5.get_LAST_ARRAY() != 0 ||
		shader->textureUsesDepthCompare[unit])
		return false;
	const uint32 firstMip = resource.word4.get_BASE_LEVEL();
	const uint32 lastMip = resource.word5.get_LAST_LEVEL();
	const uint32 largestDimension = std::max(resource.word0.get_WIDTH() + 1, resource.word1.get_HEIGHT() + 1);
	if (lastMip < firstMip || lastMip == 0 || lastMip >= 16 || (largestDimension >> lastMip) == 0)
		return false;
	const uint32 sampler = shader->textureUnitSamplerAssignment[unit];
	if (sampler == LATTE_DECOMPILER_SAMPLER_NONE)
		return false;
	const auto& lod = LatteGPUState.contextNew.SQ_TEX_SAMPLER[sampler + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType)].WORD1;
	// Batman streams level zero separately, keeping it inaccessible with MIN_LOD=1.
	return lod.get_MAX_LOD() >= lod.get_MIN_LOD() && (firstMip > 0 || lod.get_MIN_LOD() >= 64);
}

static bool IsMN9IncompleteFaceAtlas(const LatteDecompilerShader* shader, sint32 unit,
	const _LatteRegisterSetTextureUnit* resources)
{
	if (CafeSystem::GetForegroundTitleId() != 0x00050000101c9600ull ||
		CafeSystem::GetForegroundTitleVersion() != 16 || CafeSystem::GetRPXHashUpdated() != 0x73e4ffc8 ||
		shader->shaderType != LatteConst::ShaderType::Pixel)
		return false;
	sint32 diffuse, alternate;
	if (shader->baseHash == 0xa75fbd92361700c6ull)
	{
		diffuse = 0;
		alternate = 1;
	}
	else if (shader->baseHash == 0x312187c7df729ffdull)
	{
		diffuse = 2;
		alternate = 4;
	}
	else if (shader->baseHash == 0xe74f2dbd9db23d02ull)
	{
		diffuse = 3;
		alternate = 5;
	}
	else
		return false;
	const auto& r = resources[unit];
	// The captured mouth atlas has a valid base but unrelated lower-mip data.
	// Limit this compatibility workaround to its shared diffuse/alternate binding.
	return (unit == diffuse || unit == alternate) &&
		r.word0.get_WIDTH() + 1 == 256 && r.word1.get_HEIGHT() + 1 == 512 &&
		r.word0.get_DIM() == Latte::E_DIM::DIM_2D && r.word1.get_DEPTH() == 0 &&
		r.word4.get_BASE_LEVEL() == 0 && r.word5.get_LAST_LEVEL() == 9 &&
		LatteTexture_ReconstructGX2Format(r.word1, r.word4) == Latte::E_GX2SURFFMT::BC1_SRGB &&
		resources[diffuse].word2.get_BASE_ADDRESS() == resources[alternate].word2.get_BASE_ADDRESS();
}

void LatteTexture_updateTexturesForStage(LatteDecompilerShader* shaderContext, uint32 glBackendBaseTexUnit, _LatteRegisterSetTextureUnit* texRegBase)
{
	const bool swapVideoShader = shaderContext->shaderType == LatteConst::ShaderType::Pixel &&
		shaderContext->baseHash == 0xa966efe7df7be36dull &&
		SwapForcePublishedVideo::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
	const auto videoFrame = swapVideoShader ? SwapForcePublishedVideo::Acquire(texRegBase[0].word2.get_BASE_ADDRESS() << 8) : nullptr;
	const bool primeVideo = shaderContext->shaderType == LatteConst::ShaderType::Pixel &&
		shaderContext->textureUnitListCount == 3 &&
		PrimePublishedVideo::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
	const auto primeFrame = primeVideo ? PrimePublishedVideo::Acquire({
		texRegBase[0].word2.get_BASE_ADDRESS() << 8, texRegBase[1].word2.get_BASE_ADDRESS() << 8,
		texRegBase[2].word2.get_BASE_ADDRESS() << 8}) : nullptr;
	for (sint32 z = 0; z < shaderContext->textureUnitListCount; z++)
	{
		sint32 textureIndex = shaderContext->textureUnitList[z];
		const auto& texRegister = texRegBase[textureIndex];

		// get physical address of texture data
		MPTR physAddr = (texRegister.word2.get_BASE_ADDRESS() << 8);
		const bool traceBatmanFace = shaderContext->baseHash == 0x1333635f8ccb900bull &&
			CafeSystem::GetForegroundTitleId() == 0x0005000010137C00ull &&
			CompatibilityDiagnostics::IsEnabled("batmantex", CafeSystem::GetForegroundTitleId());
		if (traceBatmanFace)
		{
			static uint32 lastAddress[Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE]{};
			static bool seen[Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE]{};
			static uint32 count = 0;
			if (count < 128 && (!seen[textureIndex] || lastAddress[textureIndex] != physAddr))
			{
				seen[textureIndex] = true;
				lastAddress[textureIndex] = physAddr;
				++count;
				const auto samplerAssignment = shaderContext->textureUnitSamplerAssignment[textureIndex];
				uint32 samplerWord1 = 0;
				if (samplerAssignment != LATTE_DECOMPILER_SAMPLER_NONE)
					samplerWord1 = LatteGPUState.contextNew.SQ_TEX_SAMPLER[samplerAssignment + LatteDecompiler_getTextureSamplerBaseIndex(shaderContext->shaderType)].WORD1.getRawValue();
				cemuLog_log(LogType::Force, "[BATMAN:TEX] frame={} shader={:016x} unit={} physical={:08x} mip={:08x} size={}x{} format={:x} type={} first={} last={} sampler={} lodraw={:08x}",
					LatteGPUState.frameCounter, shaderContext->baseHash, textureIndex, physAddr,
					texRegister.word3.get_MIP_ADDRESS() << 8, texRegister.word0.get_WIDTH() + 1,
					texRegister.word1.get_HEIGHT() + 1, (uint32)texRegister.word1.get_DATA_FORMAT(), (uint32)texRegister.word6.get_TYPE(),
					texRegister.word4.get_BASE_LEVEL(), texRegister.word5.get_LAST_LEVEL(), samplerAssignment, samplerWord1);
			}
		}
		const bool mipOnly = physAddr == MPTR_NULL && IsBatmanResidentMipChain(shaderContext, textureIndex, texRegister);
		if (physAddr == MPTR_NULL && !mipOnly)
			continue; // invalid data
		MPTR physMipAddr = (texRegister.word3.get_MIP_ADDRESS() << 8);

		// word0
		const auto word0 = texRegister.word0;
		auto dim = word0.get_DIM();
		uint32 pitch = (word0.get_PITCH() + 1) << 3;
		uint32 width = word0.get_WIDTH() + 1;
		auto tileMode = word0.get_TILE_MODE();
		// word1
		const auto word1 = texRegister.word1;
		uint32 depth = word1.get_DEPTH();
		if (dim == Latte::E_DIM::DIM_2D_ARRAY || dim == Latte::E_DIM::DIM_3D || dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA || dim == Latte::E_DIM::DIM_1D_ARRAY)
		{
			depth = depth + 1;
		}
		else
		{
			if (dim == Latte::E_DIM::DIM_CUBEMAP)
				depth = 6 * (depth + 1);
			if (depth == 0)
				depth = 1;
		}
		uint32 height = word1.get_HEIGHT() + 1;
		if (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY)
			height = 1;
		if (Latte::IsCompressedFormat(word1.get_DATA_FORMAT()))
			pitch /= 4;
		// view slice
		const auto word4 = texRegister.word4;
		const auto word5 = texRegister.word5;

		uint32 viewFirstSlice = word5.get_BASE_ARRAY();
		uint32 viewNumSlices = word5.get_LAST_ARRAY() + 1 - viewFirstSlice;

		uint32 viewFirstMip = word4.get_BASE_LEVEL();
		uint32 viewNumMips = word5.get_LAST_LEVEL() + 1 - viewFirstMip;
		if (IsMN9IncompleteFaceAtlas(shaderContext, textureIndex, texRegBase))
		{
			viewNumMips = 1;
			cemuLog_logOnce(LogType::Force, "Mighty No. 9: using resident base level for the incomplete face atlas");
		}

		cemu_assert_debug(viewNumMips != 0);

		Latte::E_GX2SURFFMT format = LatteTexture_ReconstructGX2Format(word1, word4);
		if (CafeSystem::GetForegroundTitleId() == 0x0005000010134e00ull &&
			format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM && width == 256 && height == 1024 &&
			CompatibilityDiagnostics::IsEnabled("sniperlut", CafeSystem::GetForegroundTitleId()))
		{
			cemuLog_logOnce(LogType::Force, "[SNIPER:LUT] shader={:016x} unit={} address={:08x} pitch={} endian={} raw4={:08x}",
				shaderContext->baseHash, textureIndex, physAddr, pitch, (uint32)word4.get_ENDIAN_SWAP(), word4.getRawValue());
		}
		if (Nba2K13Compatibility::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) &&
			shaderContext->shaderType == LatteConst::ShaderType::Pixel &&
			Nba2K13Compatibility::IsGeneratedPlayerMaterial(shaderContext->baseHash, textureIndex) &&
			format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB && dim == Latte::E_DIM::DIM_2D &&
			width == 256 && height == 256 && pitch == 256 && depth == 1 &&
			viewFirstMip == 0 && viewNumMips == 9 && viewFirstSlice == 0 && viewNumSlices == 1 &&
			physMipAddr == physAddr + 0x40000)
		{
			// MyPLAYER's generated skin/hair has a valid base but placeholder lower mips.
			// Keep this resident-level workaround local to the captured material layout.
			viewNumMips = 1;
		}

		// todo - AA
		if (dim == Latte::E_DIM::DIM_2D_MSAA)
		{
			// MSAA only supports one mip level?
			// without this we encounter a crash in The Mysterious Cities of Gold: Secret Paths due to it setting mip count to 2 and leaving mip pointer on an invalid uninitialized value
			viewFirstMip = 0;
			viewNumMips = 1;
		}

		// swizzle
		uint32 swizzle = 0;
		if (Latte::TM_IsMacroTiled(tileMode))
		{
			// extract swizzle bits from pointer if macro-tiled
			swizzle = (physAddr & 0x700);
			physAddr &= ~0x700;
		}

		bool isDepthSampler = shaderContext->textureUsesDepthCompare[textureIndex];
		// look for already existing texture
		LatteTextureView* textureView;
		if (mipOnly)
			textureView = LatteTextureViewLookupCache::lookupMipOnly(physMipAddr, width, height, pitch, viewFirstMip, viewNumMips, format, tileMode);
		else if (!isDepthSampler)
			textureView = LatteTextureViewLookupCache::lookup(physAddr, width, height, depth, pitch, viewFirstMip, viewNumMips, viewFirstSlice, viewNumSlices, format, dim);
		else
			textureView = LatteTextureViewLookupCache::lookupWithColorOrDepthType(physAddr, width, height, depth, pitch, viewFirstMip, viewNumMips, viewFirstSlice, viewNumSlices, format, dim, true);
		if (!textureView)
		{
			// view not found, create a new mapping which will also create a new texture if necessary
			if (mipOnly)
			{
				textureView = LatteTexture_CreateTexture(dim, 0, physMipAddr, format, width, height, 1, pitch, viewFirstMip + viewNumMips, swizzle, tileMode, false, true);
				textureView = textureView->baseTexture->GetOrCreateView(viewFirstMip, viewNumMips, 0, 1);
				cemuLog_logOnce(LogType::Force, "Batman: binding resident mip chains without an allocated base level (title {:016x})", CafeSystem::GetForegroundTitleId());
			}
			else
				textureView = LatteTexture_CreateMapping(physAddr, physMipAddr, width, height, depth, pitch, tileMode, swizzle, viewFirstMip, viewNumMips, viewFirstSlice, viewNumSlices, format, dim, dim, isDepthSampler);
			if (textureView == nullptr)
				continue;
			LatteGPUState.repeatTextureInitialization = true;
		}

#ifdef ENABLE_OPENGL
		if (g_renderer->GetType() == RendererAPI::OpenGL)
		{
			// on OpenGL, texture views and sampler parameters are tied together (we are avoiding sampler objects due to driver bugs)
			// in order to emulate different sampler parameters when a texture is bound multiple times we create extra views
			OpenGLRenderer* rendererGL = static_cast<OpenGLRenderer*>(g_renderer.get());

			// if this texture is bound multiple times then use alternative views
			if (textureView->lastTextureBindIndex == LatteGPUState.textureBindCounter)
			{
				LatteTextureViewGL* textureViewGL = (LatteTextureViewGL*)textureView;
				// get next unused alternative texture view
				while (true)
				{
					textureViewGL = textureViewGL->GetAlternativeView();
					if (textureViewGL->lastTextureBindIndex != LatteGPUState.textureBindCounter)
						break;
				}
				textureView = textureViewGL;
		}
			textureView->lastTextureBindIndex = LatteGPUState.textureBindCounter;
			rendererGL->renderstate_updateTextureSettingsGL(shaderContext, textureView, textureIndex + glBackendBaseTexUnit, word4, textureIndex, isDepthSampler);
		}
#endif

		g_renderer->texture_setLatteTexture(textureView, textureIndex + glBackendBaseTexUnit);
		// update if data changed
		bool swizzleChanged = false;
		if (textureView->baseTexture->swizzle != swizzle)
		{
			debug_printf("BaseSwizzle diff prev %08x new %08x rt %08x tm %d\n", textureView->baseTexture->swizzle, swizzle, textureView->baseTexture->lastRenderTargetSwizzle, textureView->baseTexture->tileMode);
			if (swizzle == textureView->baseTexture->lastRenderTargetSwizzle)
			{
				// last render to texture updated the swizzle and we can assume the texture data is still valid
				textureView->baseTexture->swizzle = textureView->baseTexture->lastRenderTargetSwizzle;
			}
			else
			{
				// reload texture
				swizzleChanged = true;
			}
		}
		else if ((viewFirstMip + viewNumMips) > 1 && (textureView->baseTexture->physMipAddress != physMipAddr))
		{
			debug_printf("MipPhys/Swizzle change diff prev %08x new %08x tm %d\n", textureView->baseTexture->physMipAddress, physMipAddr, textureView->baseTexture->tileMode);
			swizzleChanged = true;
			cemu_assert_debug(physMipAddr != MPTR_NULL);
		}
		const bool publishedVideoPlane = videoFrame && textureIndex < 3 &&
			videoFrame->addresses[textureIndex] == physAddr && videoFrame->planes[textureIndex] &&
			format == Latte::E_GX2SURFFMT::R8_UNORM && tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED &&
			dim == Latte::E_DIM::DIM_2D && depth == 1 && viewFirstMip == 0 && viewNumMips == 1 &&
			!textureView->baseTexture->isUpdatedOnGPU &&
			!textureView->baseTexture->overwriteInfo.hasResolutionOverwrite &&
			!textureView->baseTexture->overwriteInfo.hasFormatOverwrite && pitch == width &&
			((textureIndex == 0 && width == 2048 && height == 1024) ||
			 (textureIndex > 0 && width == 1024 && height == 512));
		const bool primeVideoPlane = primeFrame && textureIndex < 3 &&
			primeFrame->addresses[textureIndex] == physAddr && primeFrame->planes[textureIndex] &&
			format == Latte::E_GX2SURFFMT::R8_UNORM && tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED &&
			dim == Latte::E_DIM::DIM_2D && depth == 1 && viewFirstMip == 0 && viewNumMips == 1 &&
			viewFirstSlice == 0 && viewNumSlices == 1 && !textureView->baseTexture->isUpdatedOnGPU &&
			!textureView->baseTexture->overwriteInfo.hasResolutionOverwrite &&
			!textureView->baseTexture->overwriteInfo.hasFormatOverwrite &&
			PrimePublishedVideo::IsPlane(textureIndex, width, height, pitch);
		const bool nbaVideoPlane = textureIndex == 0 &&
			shaderContext->shaderType == LatteConst::ShaderType::Pixel &&
			shaderContext->baseHash == NbaPublishedVideo::MovieShader &&
			NbaPublishedVideo::IsTarget(CafeSystem::GetForegroundTitleId(),
				CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) &&
			NbaPublishedVideo::IsLayout((uint32)format, (uint32)tileMode, (uint32)dim,
				width, height, depth, pitch, 1, 0, viewFirstMip, viewNumMips,
				viewFirstSlice, viewNumSlices) &&
			!textureView->baseTexture->isUpdatedOnGPU &&
			!textureView->baseTexture->overwriteInfo.hasResolutionOverwrite &&
			!textureView->baseTexture->overwriteInfo.hasFormatOverwrite;
		const auto nbaFrame = nbaVideoPlane ? NbaPublishedVideo::frames.Acquire(physAddr) : nullptr;
		if (nbaFrame)
		{
			const uint32 size = width * height * 4;
			auto* upload = static_cast<uint8*>(g_renderer->texture_acquireTextureUploadBuffer(size));
			for (uint32 i = 0; i < width * height; i += 2)
			{
				const auto* pair = nbaFrame->packed.data() + i * 2;
				TextureDecoder_Packed422<false>::Unpack(pair, pair + 2, false, upload + i * 4);
				TextureDecoder_Packed422<false>::Unpack(pair, pair + 2, true, upload + (i + 1) * 4);
			}
			g_renderer->texture_loadSlice(textureView->baseTexture, width, height, 1, upload, 0, 0, size);
			g_renderer->texture_releaseTextureUploadBuffer(upload);
			textureView->baseTexture->reloadFromDynamicTextures = false;
			textureView->baseTexture->lastUpdateEventCounter = LatteTexture_getNextUpdateEventCounter();
			cemuLog_logOnce(LogType::Force, "[COMPAT:NBA-Video] Uploading immutable submitted movie image");
		}
		else if (primeVideoPlane)
		{
			const auto& bytes = *primeFrame->planes[textureIndex];
			auto* upload = static_cast<uint8*>(g_renderer->texture_acquireTextureUploadBuffer(static_cast<uint32>(bytes.size())));
			std::memcpy(upload, bytes.data(), bytes.size());
			g_renderer->texture_loadSlice(textureView->baseTexture, width, height, 1, upload, 0, 0,
				static_cast<uint32>(bytes.size()));
			g_renderer->texture_releaseTextureUploadBuffer(upload);
			textureView->baseTexture->reloadFromDynamicTextures = false;
			textureView->baseTexture->lastUpdateEventCounter = LatteTexture_getNextUpdateEventCounter();
			cemuLog_logOnce(LogType::Force, "Video: displaying submitted video planes (title {:016x}, shader {:016x})", CafeSystem::GetForegroundTitleId(), shaderContext->baseHash);
		}
		else if (publishedVideoPlane)
		{
			const auto& bytes = *videoFrame->planes[textureIndex];
			auto* upload = static_cast<uint8*>(g_renderer->texture_acquireTextureUploadBuffer(static_cast<uint32>(bytes.size())));
			std::memcpy(upload, bytes.data(), bytes.size());
			g_renderer->texture_loadSlice(textureView->baseTexture, width, height, 1, upload, 0, 0,
				static_cast<uint32>(bytes.size()));
			g_renderer->texture_releaseTextureUploadBuffer(upload);
			textureView->baseTexture->reloadFromDynamicTextures = false;
			textureView->baseTexture->lastUpdateEventCounter = LatteTexture_getNextUpdateEventCounter();
			cemuLog_logOnce(LogType::Force, "SWAP Force: rendering published video planes before decoder reuse");
		}
		else if (LatteTC_HasTextureChanged(textureView->baseTexture) || swizzleChanged)
		{
			debug_printf("Reload texture 0x%08x res %dx%d memRange %08x-%08x SwizzleChange: %s\n", textureView->baseTexture->physAddress, textureView->baseTexture->width, textureView->baseTexture->height, textureView->baseTexture->texDataPtrLow, textureView->baseTexture->texDataPtrHigh, swizzleChanged ? "yes" : "no");
			// update swizzle / changed mip address
			if (swizzleChanged)
			{
				textureView->baseTexture->swizzle = swizzle;
				if ((viewFirstMip + viewNumMips) > 1)
				{
					textureView->baseTexture->physMipAddress = physMipAddr;
				}
			}
			debug_printf("Reload reason: Data-change when bound as texture (new hash 0x%08x)\n", textureView->baseTexture->texDataHash2);
			LatteTexture_ReloadData(textureView->baseTexture);
		}
		LatteTexture* baseTexture = textureView->baseTexture;
		if (baseTexture->lastAccessFrameCount != LatteGPUState.frameCounter &&
			CompatibilityDiagnostics::IsEnabled("texture", CafeSystem::GetForegroundTitleId()) &&
			CompatibilityDiagnostics::MatchesTexture(baseTexture->width, baseTexture->height))
		{
			const LatteTextureSliceMipInfo* sliceInfo = baseTexture->GetSliceMipArrayEntry(0, 0);
			cemuLog_log(LogType::Force,
				"[COMPAT:TEXTURE] frame={} address=0x{:08x} range=0x{:08x}-0x{:08x} hash=0x{:08x}/0x{:08x} "
				"defined={} gpu={} dynamic_reload={} reloads={} write={} update={} slice_update={} relations={} overlaps={}",
				LatteGPUState.frameCounter, baseTexture->physAddress, baseTexture->texDataPtrLow, baseTexture->texDataPtrHigh,
				baseTexture->texDataHash2, LatteTexture_CalculateTextureDataHash(baseTexture),
				baseTexture->isDataDefined, baseTexture->isUpdatedOnGPU, baseTexture->reloadFromDynamicTextures,
				baseTexture->reloadCount, baseTexture->lastWriteEventCounter, baseTexture->lastUpdateEventCounter,
				sliceInfo ? sliceInfo->lastDynamicUpdate : 0, baseTexture->list_compatibleRelations.size(),
				sliceInfo ? sliceInfo->list_dataOverlap.size() : 0);
		}
		if (baseTexture->reloadFromDynamicTextures)
		{
			LatteTexture_UpdateCacheFromDynamicTextures(baseTexture);
			baseTexture->reloadFromDynamicTextures = false;
		}
		LatteTC_MarkTextureStillInUse(baseTexture);

		// check if barrier is necessary
		if ((sint32)(LatteGPUState.drawCallCounter - baseTexture->lastUnflushedRTDrawcallIndex) < 2)
		{
			LatteGPUState.requiresTextureBarrier = true;
			baseTexture->lastUnflushedRTDrawcallIndex = 0;
		}
		// update scale
		float texScaleU, texScaleV;
		if (baseTexture->overwriteInfo.hasResolutionOverwrite == false)
		{
			texScaleU = 1.0f;
			texScaleV = 1.0f;
		}
		else
		{
			texScaleU = (float)baseTexture->overwriteInfo.width / (float)baseTexture->width;
			texScaleV = (float)baseTexture->overwriteInfo.height / (float)baseTexture->height;
		}
		LatteTexture_setEffectiveTextureScale(shaderContext->shaderType, textureIndex, texScaleU, texScaleV);
	}
}

// initialize textures used by the current drawcall
// Sets LatteGPUState.repeatTextureInitialization to true if a new texture mapping was created (indicating that this function must be called again)
// also sets LatteGPUState.requiresTextureBarrier to true if texture barrier is required
void LatteTexture_updateTextures()
{
	LatteGPUState.textureBindCounter++;
	// pixel shader
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	if (pixelShader)
		LatteTexture_updateTexturesForStage(pixelShader, LATTE_CEMU_PS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_PS);
	// vertex shader
	LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
	cemu_assert_debug(vertexShader != nullptr);
	LatteTexture_updateTexturesForStage(vertexShader, LATTE_CEMU_VS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_VS);
	// geometry shader
	LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
	if (geometryShader)
		LatteTexture_updateTexturesForStage(geometryShader, LATTE_CEMU_GS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_GS);
}

sint32 LatteTexture_getEffectiveWidth(LatteTexture* texture)
{
	if (texture->overwriteInfo.hasResolutionOverwrite)
		return texture->overwriteInfo.width;
	return texture->width;
}

// returns true if the two textures have the same rescale factor
bool LatteTexture_doesEffectiveRescaleRatioMatch(LatteTexture* texture1, sint32 mipLevel1, LatteTexture* texture2, sint32 mipLevel2)
{
	double widthRatio1 = (double)LatteTexture_getEffectiveWidth(texture1) / (double)texture1->width;
	double widthRatio2 = (double)LatteTexture_getEffectiveWidth(texture2) / (double)texture2->width;
	// the difference between the factors must be less than 5%
	double diff = widthRatio1 / widthRatio2;
	if (abs(1.0 - diff) > 0.05)
	{
		return false;
	}
	return true;
}

void LatteTexture_scaleToEffectiveSize(LatteTexture* texture, sint32* x, sint32* y, sint32 mipLevel)
{
	if( texture->overwriteInfo.hasResolutionOverwrite == false )
		return;
	*x = *x * std::max(1,texture->overwriteInfo.width>>mipLevel) / std::max(1,texture->width>>mipLevel);
	*y = *y * std::max(1,texture->overwriteInfo.height>>mipLevel) / std::max(1, texture->height>>mipLevel);
}

uint64 _textureUpdateEventCounter = 1;

uint64 LatteTexture_getNextUpdateEventCounter()
{
	uint64 counter = _textureUpdateEventCounter;
	_textureUpdateEventCounter++;
	return counter;
}

void LatteTexture_init()
{
}
