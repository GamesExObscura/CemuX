#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Latte/Core/SmurfsParticleSampler.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "config/ActiveSettings.h"

// generate a Cemu version and setting dependent id
uint32 RendererShader::GeneratePrecompiledCacheId()
{
	uint32 v = 0;
	const char* s = EMULATOR_VERSION_SUFFIX;
	while (*s)
	{
		v = std::rotl<uint32>(v, 7);
		v += (uint32)(*s);
		s++;
	}
	v += (EMULATOR_VERSION_MAJOR * 1000000u);
	v += (EMULATOR_VERSION_MINOR * 10000u);
	v += (EMULATOR_VERSION_PATCH * 100u);

	// settings that can influence shaders
	v += (uint32)g_current_game_profile->GetAccurateShaderMul() * 133;

	return v;
}

void RendererShader::GenerateShaderPrecompiledCacheFilename(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, uint64& h1, uint64& h2)
{
	h1 = baseHash;
	h2 = auxHash;

	if (type == RendererShader::ShaderType::kVertex)
		h2 += 0xA16374cull;
	else if (type == RendererShader::ShaderType::kFragment)
		h2 += 0x8752deull;
	else if (type == RendererShader::ShaderType::kGeometry)
		h2 += 0x65a035ull;

	// Rebuild Injustice binaries that previously omitted repeat-loop counts.
	if (ActiveSettings::UseInjusticeLoopConstants())
		h2 ^= 0x1A1C0A5700000001ull;

	// Keep the corrected AC3 shader separate from pre-fix OpenGL/SPIR-V binaries.
	if (type == RendererShader::ShaderType::kVertex && ActiveSettings::UseAC3ClothSamplerPrecision(baseHash))
		h2 ^= 0xAC3C10A100000001ull;

	if (type == RendererShader::ShaderType::kVertex &&
		SmurfsParticleSampler::UseRowPrecision(CafeSystem::GetForegroundTitleId(),
			CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated(), baseHash))
		h2 ^= SmurfsParticleSampler::CacheSalt;

	// Do not reuse pre-fix GL/SPIR-V binaries; other titles keep their cache keys.
	if (ActiveSettings::UseDisneyInfinityLogicalShift())
		h2 ^= 0xD13A15A100000001ull;

	// Retain other shaders' binaries while rebuilding Epic Mickey's grading lookup.
	if (type == RendererShader::ShaderType::kFragment && ActiveSettings::UseEpicMickeyColorVolume(baseHash))
		h2 ^= 0xE91C001000000001ull;

	// Only FIFA's mip generator must discard its pre-fix GL/SPIR-V binary.
	if (type == RendererShader::ShaderType::kFragment && ActiveSettings::UseFifaCrowdRegisterLodBias(baseHash))
		h2 ^= 0xF1FA001300000001ull;
}
