#pragma once
#include "Cafe/OS/RPL/COSModule.h"
#include <functional>
#include <array>

namespace nn::nfp
{
	uint32 NFCGetTagInfo(uint32 index, uint32 timeout, MPTR functionPtr, void* userParam);

	COSModule* GetModule();
}

void nnNfp_load();
void nnNfp_update();

bool nnNfp_isInitialized();
bool nnNfp_touchNfcTagFromFile(const fs::path& filePath, uint32* nfcError);
inline constexpr std::array<uint64, 3> nnNfpAmiiboSettingsTitles{
	0x000500301006D10Aull, 0x000500301006D00Aull, 0x000500301006D20Aull};

constexpr bool nnNfp_isAmiiboSettingsTitle(uint64 titleId)
{
	for (const auto candidate : nnNfpAmiiboSettingsTitles)
		if (candidate == titleId)
			return true;
	return false;
}

bool nnNfp_prepareRegistrationShortcut(uint64 titleId, std::function<void()> onReturn);
void nnNfp_cancelRegistrationShortcut();

#define NFP_STATE_NONE			(0)
#define NFP_STATE_INIT			(1)
#define NFP_STATE_RW_SEARCH		(2)
#define NFP_STATE_RW_ACTIVE		(3)
#define NFP_STATE_RW_DEACTIVE	(4)
#define NFP_STATE_RW_MOUNT		(5)
#define NFP_STATE_UNEXPECTED	(6)
#define NFP_STATE_RW_MOUNT_ROM	(7)
