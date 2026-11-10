#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/CafeSystem.h"
#include "Cemu/Debug/CompatibilityDiagnostics.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Texture.h"
#include "Cafe/HW/Latte/Core/SwapForcePublishedVideo.h"
#include "Cafe/HW/Latte/Core/PrimePublishedVideo.h"
#include "Cafe/HW/Latte/Core/NbaPublishedVideo.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

namespace GX2
{
	using namespace Latte;

	namespace
	{
		void TraceTexture(const char* event, const GX2Texture* texture, uint32 baseRegister = 0,
			uint32 textureUnit = 0, MPTR effectiveImage = MPTR_NULL)
		{
			const GX2Surface& surface = texture->surface;
			const uint64 titleId = CafeSystem::GetForegroundTitleId();
			const bool traceGX2 = CompatibilityDiagnostics::IsEnabled("gx2", titleId);
			const bool traceCallsite = CompatibilityDiagnostics::IsEnabled("callsite", titleId);
			if ((!traceGX2 && !traceCallsite) ||
				!CompatibilityDiagnostics::MatchesTexture(surface.width, surface.height))
			{
				return;
			}

			CompatibilityDiagnostics::TrackTextureRange(surface.imagePtr, surface.imageSize);
			CompatibilityDiagnostics::TrackTextureRange(surface.mipPtr, surface.mipSize);
			if (effectiveImage == MPTR_NULL)
				effectiveImage = surface.imagePtr;
			const uint32 physicalAddress = memory_virtualToPhysical(effectiveImage);
			CompatibilityDiagnostics::TrackTextureRange(physicalAddress, surface.imageSize);
			if (traceGX2)
			{
				cemuLog_log(LogType::Force,
					"[COMPAT:GX2] event={} size={}x{} format=0x{:04x} pitch={} levels={} image=0x{:08x} physical=0x{:08x} image_size={} "
					"mip=0x{:08x} mip_size={} resource_flags=0x{:08x} base_register=0x{:08x} unit={}",
					event, (uint32)surface.width, (uint32)surface.height, (uint32)surface.format.value(),
					(uint32)surface.pitch, (uint32)surface.numLevels, (uint32)surface.imagePtr, physicalAddress,
					(uint32)surface.imageSize, (uint32)surface.mipPtr, (uint32)surface.mipSize,
					(uint32)surface.resFlag, baseRegister, textureUnit);
			}
			if (traceCallsite && std::string_view{ event } == "bind_texture")
			{
				PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
				cemuLog_log(LogType::Force,
					"[COMPAT:CALLSITE] event=bind_texture lr=0x{:08x} ip=0x{:08x} sp=0x{:08x} image=0x{:08x}",
					hCPU->spr.LR, hCPU->instructionPointer, hCPU->gpr[1], physicalAddress);
				static std::once_flag s_logStackOnce;
				std::call_once(s_logStackOnce, [hCPU]
				{
					DebugLogStackTrace(coreinit::OSGetCurrentThread(), hCPU->gpr[1]);
				});
			}
		}
	}

	/****** Texture functions ******/

	void GX2InitTextureRegs(GX2Texture* texture)
	{
		uint32 _regs[5] = { 0 };

		// some values may not be zero
		if (texture->viewNumMips == 0)
			texture->viewNumMips = 1;
		if (texture->viewNumSlices == 0)
			texture->viewNumSlices = 1;

		if (texture->surface.height == 0)
			texture->surface.height = 1;
		if (texture->surface.depth == 0)
			texture->surface.depth = 1;
		if (texture->surface.numLevels == 0)
			texture->surface.numLevels = 1;

		// texture parameters
		uint32 viewNumMips = texture->viewNumMips;
		uint32 viewNumSlices = texture->viewNumSlices;
		uint32 viewFirstMip = texture->viewFirstMip;
		uint32 viewFirstSlice = texture->viewFirstSlice;
		uint32 compSel = texture->compSel;

		// surface parameters
		uint32 width = texture->surface.width;
		uint32 height = texture->surface.height;
		uint32 depth = texture->surface.depth;
		uint32 pitch = texture->surface.pitch;
		uint32 numMips = texture->surface.numLevels;
		Latte::E_GX2SURFFMT format = texture->surface.format;
		Latte::E_DIM dim = texture->surface.dim;
		uint32 tileMode = (uint32)texture->surface.tileMode.value();
		uint32 surfaceFlags = texture->surface.resFlag;
		uint32 surfaceAA = texture->surface.aa;

		// calculate register word 0
		Latte::E_HWSURFFMT formatHw = Latte::GetHWFormat(format);

		Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N newRegWord0;
		newRegWord0.set_DIM(dim);
		newRegWord0.set_TILE_MODE(Latte::MakeHWTileMode(texture->surface.tileMode));
		newRegWord0.set_TILE_TYPE((surfaceFlags&4) != 0);

		uint32 pixelPitch = pitch;
		if (Latte::IsCompressedFormat(formatHw))
			pixelPitch *= 4;

		if(pixelPitch == 0)
			newRegWord0.set_PITCH(0x7FF);
		else
			newRegWord0.set_PITCH((pixelPitch >> 3) - 1);

		if (width == 0)
			newRegWord0.set_WIDTH(0x1FFF);
		else
			newRegWord0.set_WIDTH(width - 1);

		texture->regTexWord0 = newRegWord0;

		// calculate register word 1
		Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N newRegWord1;
		newRegWord1.set_HEIGHT(height - 1);

		if (dim == Latte::E_DIM::DIM_CUBEMAP)
		{
			newRegWord1.set_DEPTH((depth / 6) - 1);
		}
		else if (dim == E_DIM::DIM_3D ||
			dim == E_DIM::DIM_2D_ARRAY_MSAA ||
			dim == E_DIM::DIM_2D_ARRAY ||
			dim == E_DIM::DIM_1D_ARRAY)
		{
			newRegWord1.set_DEPTH(depth - 1);
		}
		else
		{
			newRegWord1.set_DEPTH(0);
		}
		newRegWord1.set_DATA_FORMAT(formatHw);
		texture->regTexWord1 = newRegWord1;

		// calculate register word 2
		LATTE_SQ_TEX_RESOURCE_WORD4_N newRegWord4;

		LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP formatComp;
		if (HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_SIGNED))
			formatComp = LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP::COMP_SIGNED;
		else
			formatComp = LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP::COMP_UNSIGNED;
		newRegWord4.set_FORMAT_COMP_X(formatComp);
		newRegWord4.set_FORMAT_COMP_Y(formatComp);
		newRegWord4.set_FORMAT_COMP_Z(formatComp);
		newRegWord4.set_FORMAT_COMP_W(formatComp);

		if (HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_FLOAT))
			newRegWord4.set_NUM_FORM_ALL(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_SCALED);
		else if (HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_INT))
			newRegWord4.set_NUM_FORM_ALL(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_INT);
		else
			newRegWord4.set_NUM_FORM_ALL(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_NORM);

		if (HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_SRGB))
			newRegWord4.set_FORCE_DEGAMMA(true);

		newRegWord4.set_ENDIAN_SWAP(GX2::GetSurfaceFormatSwapMode((Latte::E_GX2SURFFMT)format));

		newRegWord4.set_REQUEST_SIZE(2);

		newRegWord4.set_DST_SEL_X((Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL)((compSel >> 24) & 0x7));
		newRegWord4.set_DST_SEL_Y((Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL)((compSel >> 16) & 0x7));
		newRegWord4.set_DST_SEL_Z((Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL)((compSel >> 8) & 0x7));
		newRegWord4.set_DST_SEL_W((Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL)((compSel >> 0) & 0x7));

		newRegWord4.set_BASE_LEVEL(viewFirstMip);
		texture->regTexWord4 = newRegWord4;

		// calculate register word 3
		LATTE_SQ_TEX_RESOURCE_WORD5_N newRegWord5;
		newRegWord5.set_LAST_LEVEL(viewFirstMip + viewNumMips - 1);
		newRegWord5.set_BASE_ARRAY(viewFirstSlice);
		newRegWord5.set_LAST_ARRAY(viewFirstSlice + viewNumSlices - 1);
		if (dim == Latte::E_DIM::DIM_CUBEMAP && ((depth / 6) - 1) != 0)
			newRegWord5.set_UKN_BIT_30(true);
		if(surfaceAA >= 1 && surfaceAA <= 3)
			newRegWord5.set_LAST_LEVEL(surfaceAA);
		texture->regTexWord5 = newRegWord5;
		// calculate register word 4
		LATTE_SQ_TEX_RESOURCE_WORD6_N newRegWord6;
		newRegWord6.set_MAX_ANISO(4);
		newRegWord6.set_PERF_MODULATION(7);
		newRegWord6.set_TYPE(Latte::LATTE_SQ_TEX_RESOURCE_WORD6_N::E_TYPE::VTX_VALID_TEXTURE);
		texture->regTexWord6 = newRegWord6;
		TraceTexture("init_texture", texture);
	}

	void _GX2SetTexture(GX2Texture* tex, Latte::REGADDR baseRegister, uint32 textureUnitIndex)
	{
		GX2ReserveCmdSpace(2 + 7);

		MPTR imagePtr = tex->surface.imagePtr;
		MPTR mipPtr = tex->surface.mipPtr;
		if (mipPtr == MPTR_NULL)
			mipPtr = imagePtr;

		uint32 swizzle = tex->surface.swizzle;

		cemu_assert_debug((swizzle & 0xFF) == 0); // does the low byte in swizzle field have any meaning?

		if (Latte::TM_IsMacroTiled(tex->surface.tileMode))
		{
			uint32 swizzleStopLevel = (swizzle >> 16) & 0xFF;
			// combine swizzle with image ptr if base level is macro tiled
			if (swizzleStopLevel > 0)
				imagePtr ^= (swizzle & 0xFFFF);
			// combine swizzle with mip ptr if first mip (level 1) is macro tiled
			if (swizzleStopLevel > 1)
				mipPtr ^= (swizzle & 0xFFFF);
		}
		TraceTexture("bind_texture", tex, (uint32)baseRegister, textureUnitIndex, imagePtr);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_RESOURCE, 8),
			baseRegister + textureUnitIndex * 7 - mmSQ_TEX_RESOURCE_WORD0,
			tex->regTexWord0,
			tex->regTexWord1,
			memory_virtualToPhysical(imagePtr) >> 8,
			memory_virtualToPhysical(mipPtr) >> 8,
			tex->regTexWord4,
			tex->regTexWord5,
			tex->regTexWord6);
	}

	void GX2SetPixelTexture(GX2Texture* tex, uint32 texUnit)
	{
		cemu_assert_debug(texUnit < Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
		const auto& s = tex->surface;
		if (texUnit == 0 && NbaPublishedVideo::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) &&
			NbaPublishedVideo::IsLayout((uint32)s.format.value(), (uint32)s.tileMode.value(), (uint32)s.dim.value(),
				s.width, s.height, s.depth, s.pitch, s.numLevels, s.aa,
				tex->viewFirstMip, tex->viewNumMips, tex->viewFirstSlice, tex->viewNumSlices) &&
			s.imageSize >= NbaPublishedVideo::PackedBytes &&
			memory_isAddressRangeAccessible(s.imagePtr, NbaPublishedVideo::PackedBytes))
		{
			// Preserve the submitted image before the CPU reuses its movie buffer.
			NbaPublishedVideo::frames.Submit(memory_virtualToPhysical(s.imagePtr),
				static_cast<const uint8*>(memory_getPointerFromVirtualOffset(s.imagePtr)),
				NbaPublishedVideo::PackedBytes);
			cemuLog_logOnce(LogType::Force, "[COMPAT:NBA-Video] Preserving submitted packed movie images");
		}
		if (PrimePublishedVideo::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) &&
			s.format == Latte::E_GX2SURFFMT::R8_UNORM && s.tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_ALIGNED &&
			s.dim == Latte::E_DIM::DIM_2D && s.depth == 1 && s.numLevels == 1 &&
			tex->viewFirstMip == 0 && tex->viewNumMips == 1 &&
			PrimePublishedVideo::IsPlane(texUnit, s.width, s.height, s.pitch))
			PrimePublishedVideo::Submit(texUnit, memory_virtualToPhysical(s.imagePtr),
				static_cast<const uint8*>(memory_getPointerFromVirtualOffset(s.imagePtr)), s.width, s.height, s.pitch);
		if (SwapForcePublishedVideo::IsTarget(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated()) && texUnit < 3 &&
			s.format == Latte::E_GX2SURFFMT::R8_UNORM && s.tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_ALIGNED &&
			s.dim == Latte::E_DIM::DIM_2D && s.depth == 1 && s.numLevels == 1 &&
			s.pitch == s.width && tex->viewFirstMip == 0 && tex->viewNumMips == 1 &&
			((texUnit == 0 && s.width == 2048 && s.height == 1024) ||
			 (texUnit > 0 && s.width == 1024 && s.height == 512)))
			SwapForcePublishedVideo::Register(texUnit, memory_virtualToPhysical(s.imagePtr));
		_GX2SetTexture(tex, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, texUnit);
	}

	void GX2SetVertexTexture(GX2Texture* tex, uint32 texUnit)
	{
		cemu_assert_debug(texUnit < Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
		_GX2SetTexture(tex, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS, texUnit);
	}

	void GX2SetGeometryTexture(GX2Texture* tex, uint32 texUnit)
	{
		cemu_assert_debug(texUnit < Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
		_GX2SetTexture(tex, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS, texUnit);
	}

	void GX2SetComputeTexture(GX2Texture* tex, uint32 texUnit)
	{
		GX2SetVertexTexture(tex, texUnit);
	}

	/****** Sampler functions ******/

	void GX2InitSampler(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampXYZ, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER filterMinMag)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0{};
		word0.set_CLAMP_X(clampXYZ).set_CLAMP_Y(clampXYZ).set_CLAMP_Z(clampXYZ);
		word0.set_XY_MAG_FILTER(filterMinMag).set_XY_MIN_FILTER(filterMinMag);
		word0.set_Z_FILTER(LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::POINT);
		word0.set_MIP_FILTER(LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::POINT);
		word0.set_TEX_ARRAY_OVERRIDE(true);

		LATTE_SQ_TEX_SAMPLER_WORD1_0 word1{};
		word1.set_MAX_LOD(0x3FF);

		LATTE_SQ_TEX_SAMPLER_WORD2_0 word2{};
		word2.set_TYPE(LATTE_SQ_TEX_SAMPLER_WORD2_0::E_SAMPLER_TYPE::UKN1);

		sampler->word0 = word0;
		sampler->word1 = word1;
		sampler->word2 = word2;
	}

	void GX2InitSamplerXYFilter(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER magFilter, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER minFilter, uint32 maxAnisoRatio)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		if (maxAnisoRatio == 0)
		{
			word0.set_XY_MAG_FILTER(magFilter);
			word0.set_XY_MIN_FILTER(minFilter);
			word0.set_MAX_ANISO_RATIO(0);
		}
		else
		{
			auto getAnisoFilter = [](LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER filter) -> LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER
			{
				if (filter == LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT)
					return LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT;
				else if (filter == LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::BILINEAR)
					return LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_BILINEAR;
				else
					cemu_assert_debug(false);
				return LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT;
			};
			word0.set_XY_MAG_FILTER(getAnisoFilter(magFilter));
			word0.set_XY_MIN_FILTER(getAnisoFilter(minFilter));
			word0.set_MAX_ANISO_RATIO(maxAnisoRatio);
		}
		sampler->word0 = word0;
	}

	void GX2InitSamplerZMFilter(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER zFilter, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER mipFilter)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		word0.set_Z_FILTER(zFilter);
		word0.set_MIP_FILTER(mipFilter);
		sampler->word0 = word0;
	}

	void GX2InitSamplerLOD(GX2Sampler* sampler, float minLod, float maxLod, float lodBias)
	{
		// known special cases: Mario & Sonic Rio passes minimum and maximum float values for minLod/maxLod
		if (minLod < 0.0)
			minLod = 0.0;
		if (maxLod > 16.0)
			maxLod = 16.0;

		uint32 iMinLod = ((uint32)floorf(minLod * 64.0f));
		uint32 iMaxLod = ((uint32)floorf(maxLod * 64.0f));
		sint32 iLodBias = (sint32)((sint32)floorf(lodBias * 64.0f)); // input range: -32.0 to 32.0
		iMinLod = std::clamp(iMinLod, 0u, 1023u);
		iMaxLod = std::clamp(iMaxLod, 0u, 1023u);
		iLodBias = std::clamp(iLodBias, -2048, 2047);

		LATTE_SQ_TEX_SAMPLER_WORD1_0 word1 = sampler->word1;
		word1.set_MIN_LOD(iMinLod);
		word1.set_MAX_LOD(iMaxLod);
		word1.set_LOD_BIAS(iLodBias);

		sampler->word1 = word1;
	}

	void GX2InitSamplerClamping(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampX, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampY, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampZ)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		word0.set_CLAMP_X(clampX);
		word0.set_CLAMP_Y(clampY);
		word0.set_CLAMP_Z(clampZ);
		sampler->word0 = word0;
	}

	void GX2InitSamplerBorderType(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE borderColorType)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		word0.set_BORDER_COLOR_TYPE(borderColorType);
		sampler->word0 = word0;
	}

	void GX2InitSamplerDepthCompare(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_DEPTH_COMPARE depthCompareFunction)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		word0.set_DEPTH_COMPARE_FUNCTION(depthCompareFunction);
		sampler->word0 = word0;
	}

	void _GX2SetSampler(GX2Sampler* sampler, uint32 samplerIndex)
	{
		GX2ReserveCmdSpace(5);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_SAMPLER, 1 + 3),
			samplerIndex * 3,
			sampler->word0, sampler->word1, sampler->word2);
	}

	void GX2SetPixelSampler(GX2Sampler* sampler, uint32 samplerIndex)
	{
		_GX2SetSampler(sampler, samplerIndex + SAMPLER_BASE_INDEX_PIXEL);
	}

	void GX2SetVertexSampler(GX2Sampler* sampler, uint32 vertexSamplerIndex)
	{
		_GX2SetSampler(sampler, vertexSamplerIndex + SAMPLER_BASE_INDEX_VERTEX);
	}

	void GX2SetGeometrySampler(GX2Sampler* sampler, uint32 geometrySamplerIndex)
	{
		_GX2SetSampler(sampler, geometrySamplerIndex + SAMPLER_BASE_INDEX_GEOMETRY);
	}

	void GX2SetComputeSampler(GX2Sampler* sampler, uint32 computeSamplerIndex)
	{
		_GX2SetSampler(sampler, computeSamplerIndex + SAMPLER_BASE_INDEX_VERTEX); // uses vertex shader stage
	}

	void GX2SetSamplerBorderColor(uint32 registerBaseOffset, uint32 samplerIndex, float red, float green, float blue, float alpha)
	{
		GX2ReserveCmdSpace(6);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONFIG_REG, 1 + 4),
			registerBaseOffset + samplerIndex * 4 - LATTE_REG_BASE_CONFIG,
			red, green, blue, alpha);
	}

	void GX2SetPixelSamplerBorderColor(uint32 pixelSamplerIndex, float red, float green, float blue, float alpha)
	{
		GX2SetSamplerBorderColor(REGADDR::TD_PS_SAMPLER0_BORDER_RED, pixelSamplerIndex, red, green, blue, alpha);
	}

	void GX2SetVertexSamplerBorderColor(uint32 vertexSamplerIndex, float red, float green, float blue, float alpha)
	{
		GX2SetSamplerBorderColor(REGADDR::TD_VS_SAMPLER0_BORDER_RED, vertexSamplerIndex, red, green, blue, alpha);
	}

	void GX2SetGeometrySamplerBorderColor(uint32 geometrySamplerIndex, float red, float green, float blue, float alpha)
	{
		GX2SetSamplerBorderColor(REGADDR::TD_GS_SAMPLER0_BORDER_RED, geometrySamplerIndex, red, green, blue, alpha);
	}

	void GX2TextureInit()
	{
		SwapForcePublishedVideo::Reset();
		PrimePublishedVideo::Reset();
		NbaPublishedVideo::frames.Reset();
		// texture
		cafeExportRegister("gx2", GX2InitTextureRegs, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPixelTexture, LogType::GX2);
		cafeExportRegister("gx2", GX2SetVertexTexture, LogType::GX2);
		cafeExportRegister("gx2", GX2SetGeometryTexture, LogType::GX2);
		cafeExportRegister("gx2", GX2SetComputeTexture, LogType::GX2);

		// sampler
		cafeExportRegister("gx2", GX2InitSampler, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerXYFilter, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerZMFilter, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerLOD, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerClamping, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerBorderType, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerDepthCompare, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPixelSampler, LogType::GX2);
		cafeExportRegister("gx2", GX2SetVertexSampler, LogType::GX2);
		cafeExportRegister("gx2", GX2SetGeometrySampler, LogType::GX2);
		cafeExportRegister("gx2", GX2SetComputeSampler, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPixelSamplerBorderColor, LogType::GX2);
		cafeExportRegister("gx2", GX2SetVertexSamplerBorderColor, LogType::GX2);
		cafeExportRegister("gx2", GX2SetGeometrySamplerBorderColor, LogType::GX2);
	}

};
