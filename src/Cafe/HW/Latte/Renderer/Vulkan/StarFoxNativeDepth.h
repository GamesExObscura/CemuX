#pragma once
#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "StarFoxDepthPolicy.h"

namespace StarFoxNativeDepth
{
inline bool deviceEnabled = false;

inline bool IsTarget()
{
    return IsTarget(CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(),
        CafeSystem::GetRPXHashUpdated());
}

inline bool Enabled()
{
    // Custom pack shaders may already remap depth; keep their pipeline convention unchanged.
    return UseNativeDepth(deviceEnabled, !GraphicPack2::GetActiveGraphicPacks().empty(),
        CafeSystem::GetForegroundTitleId(), CafeSystem::GetForegroundTitleVersion(), CafeSystem::GetRPXHashUpdated());
}

inline void SaltShaderKey(uint64& key)
{
    key = ShaderKey(key, Enabled());
}
}
