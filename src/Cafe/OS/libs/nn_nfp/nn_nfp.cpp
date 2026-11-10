#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/nn_common.h"
#include "nn_nfp.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/Account/Account.h"
#include "Common/FileStream.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/OS/libs/sysapp/sysapp.h"

namespace coreinit
{
	uint32 __LaunchByTitleId(uint64 titleId, uint32 argc, MEMPTR<char>* argv);
}

std::recursive_mutex g_nfpMutex;

void nnNfpLock()
{
	g_nfpMutex.lock();
}

bool nnNfpTryLock()
{
	return g_nfpMutex.try_lock();
}

void nnNfpUnlock()
{
	g_nfpMutex.unlock();
}

struct AmiiboInternal
{
	/* +0x000 */ uint16 lockBytes;
	/* +0x002 */ uint16 staticLock;
	/* +0x004 */ uint32 cc;
	/* +0x008 */ uint8 dataHMAC[32];
	/* +0x028 */ uint8 ukn_A5; // always 0xA5
	/* +0x029 */ uint8 writeCounterHigh;
	/* +0x029 */ uint8 writeCounterLow;
	/* +0x02B */ uint8 unk02B;
	/* encrypted region starts here */
	struct
	{
		/* +0x02C */ uint8 flags;
		/* +0x02D */ uint8 countryCode;
		/* +0x02E */ uint16be crcWriteCounter;
		/* +0x030 */ uint16be date1;
		/* +0x032 */ uint16be date2;
		/* +0x034 */ uint32be crc;
		/* +0x038 */ uint16be nickname[10];
		/* +0x04C */ uint8 mii[0x60];
		/* +0x0AC */ uint32be appDataTitleIdHigh;
		/* +0x0B0 */ uint32be appDataTitleIdLow;
		/* +0x0B4 */ uint16be appWriteCounter;
		/* +0x0B6 */ uint16be appDataIdHigh;
		/* +0x0B8 */ uint16be appDataIdLow;
		/* +0x0BA */ uint16be ukn0BA;
		/* +0x0BC */ uint32be ukn0BC;
		/* +0x0C0 */ uint32be ukn0C0;
		/* +0x0C4 */ uint32be ukn0C4;
		/* +0x0C8 */ uint32be ukn0C8;
		/* +0x0CC */ uint32be ukn0CC;
		/* +0x0D0 */ uint32be ukn0D0;
		/* +0x0D4 */ uint32be ukn0D4;
		/* +0x0D8 */ uint32be ukn0D8;
		uint32 getAppDataAppId()
		{
			uint32 appId = (uint32)appDataIdHigh << 16;
			appId |= (uint32)appDataIdLow;
			return appId;
		}

		void setAppDataAppId(uint32 appId)
		{
			appDataIdHigh = (uint16)(appId >> 16);
			appDataIdLow = (uint16)(appId & 0xFFFF);
		}
	}amiiboSettings;
	/* +0x0DC */ uint8 applicationData[0xD8];
	/* encrypted region ends here */
	/* +0x1B4 */ uint8 tagHMAC[32];
	/* +0x1D4 */ uint8 ntagSerial[7];
	/* +0x1DB */ uint8 nintendoId;
	struct
	{
		/* +0x1DC */ uint8 gameAndCharacterId[2];
		/* +0x1DE */ uint8 characterVariation;
		/* +0x1DF */ uint8 amiiboFigureType;
		/* +0x1E0 */ uint8 amiiboModelNumber[2];
		/* +0x1E2 */ uint8 amiiboSeries;
		/* +0x1E3 */ uint8 ukn_02; // always 0x02 ?
		/* +0x1E4 */ uint8 ukn5C[4];
	}amiiboIdentificationBlock;
	/* +0x1E8 */ uint8 keygenSalt[32];
};

static_assert(sizeof(AmiiboInternal) == 0x208);
static_assert(offsetof(AmiiboInternal, dataHMAC) == 0x8);
static_assert(offsetof(AmiiboInternal, amiiboSettings.appDataIdHigh) == 0xB6);
static_assert(offsetof(AmiiboInternal, amiiboSettings.ukn0D8) == 0xD8);
static_assert(offsetof(AmiiboInternal, tagHMAC) == 0x1B4);


union AmiiboRawNFCData
{
	// each page is 4 bytes
	struct
	{
		uint8 page0[4];
		uint8 page1[4];
		uint8 page2[4];
		uint8 page3[4];
		uint8 page4[4];
		uint8 page5[4];
		uint8 page6[4];
		uint8 page7[4];
		uint8 page8[4];
		uint8 page9[4];
		uint8 page10[4];
		uint8 page11[4];
		uint8 page12[4];
		uint8 page13[4];
		uint8 page14[4];
		uint8 page15[4];
		uint8 page16[4];
	};
	struct
	{
		uint8 rawByte[16 * 4];
	};
	struct
	{
		/* +0x000 */ uint8 ntagSerial[7];
		/* +0x007 */ uint8 nintendoId;
		/* +0x008 */ uint8 lockBytes[2];
		/* +0x00A */ uint8 staticLock[2];
		/* +0x00C */ uint8 cc[4]; // compatibility container
		/* +0x010 */ uint8 ukn_A5; // always 0xA5
		/* +0x011 */ uint8 writeCounter[2];
		/* +0x013 */ uint8 unk013;
		/* +0x014 */ uint8 encryptedSettings[32];
		/* +0x034 */ uint8 tagHMAC[32]; // data hmac
		/* +0x054 */
		struct
		{
			/* +0x54 */ uint8 gameAndCharacterId[2];
			/* +0x56 */ uint8 characterVariation;
			/* +0x57 */ uint8 amiiboFigureType;
			/* +0x58 */ uint8 amiiboModelNumber[2];
			/* +0x5A */ uint8 amiiboSeries;
			/* +0x5B */ uint8 ukn_02; // always 0x02 ?
			/* +0x5C */ uint8 ukn5C[4];
		}amiiboIdentificationBlock;
		/* +0x060 */ uint8 keygenSalt[32];
		/* +0x080 */ uint8 dataHMAC[32];
		/* +0x0A0 */ uint8 encryptedMii[0x60];
		/* +0x100 */ uint8 encryptedTitleId[8];
		/* +0x108 */ uint8 encryptedApplicationWriteCounter[2];
		/* +0x10A */ uint8 encryptedApplicationAreaId[4];
		/* +0x10E */ uint8 ukn10E[2];
		/* +0x110 */ uint8 unk110[32];
		/* +0x130 */ uint8 encryptedApplicationArea[0xD8];
		/* +0x208 */ uint8 dynamicLock[4];
		/* +0x20C */ uint8 cfg0[4];
		/* +0x210 */ uint8 cfg1[4];
	};
};

static_assert(sizeof(AmiiboRawNFCData) == 532);

struct AmiiboProcessedData
{
	uint8 uidLength;
	uint8 uid[7];
};

struct  
{
	bool nfpIsInitialized;
	MPTR activateEvent;
	MPTR deactivateEvent;
	bool isDetecting;
	bool isMounted;
	bool isReadOnly;
	bool hasOpenApplicationArea; // set to true if application area was opened or created
	// currently active Amiibo
	bool hasActiveAmiibo;
	fs::path amiiboPath;
	bool hasInvalidHMAC;
	uint32 amiiboTouchTime;
	AmiiboRawNFCData amiiboNFCData; // raw data
	AmiiboInternal amiiboInternal; // decrypted internal format
	AmiiboProcessedData amiiboProcessedData;
}nfp_data = { 0 };

bool nnNfp_writeCurrentAmiibo();

static bool nnNfpIsSystemSettingsTitle()
{
	return CafeSystem::GetForegroundTitleId() == 0x0005001010047100ull;
}

#include "AmiiboCrypto.h"

void nnNfpExport_SetActivateEvent(PPCInterpreter_t* hCPU)
{
	// parameters:
	// r3	OSEvent_t*
	ppcDefineParamStructPtr(osEvent, coreinit::OSEvent, 0);
	ppcDefineParamMPTR(osEventMPTR, 0);

	debug_printf("nn_nfp.SetActivateEvent(0x%08x)\n", osEventMPTR);

	coreinit::OSInitEvent(osEvent, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);

	nnNfpLock();
	nfp_data.activateEvent = osEventMPTR;
	nnNfpUnlock();

	osLib_returnFromFunction(hCPU, 0); // return value ukn
}

void nnNfpExport_SetDeactivateEvent(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(osEvent, coreinit::OSEvent, 0);
	ppcDefineParamMPTR(osEventMPTR, 0);

	cemuLog_log(LogType::NN_NFP, "SetDeactivateEvent(0x{:08x})", osEventMPTR);

	coreinit::OSInitEvent(osEvent, coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);

	nnNfpLock();
	nfp_data.deactivateEvent = osEventMPTR;
	nnNfpUnlock();

	osLib_returnFromFunction(hCPU, 0); // return value ukn
}

void nnNfpExport_Initialize(PPCInterpreter_t* hCPU)
{
	debug_printf("Nfp Initialize()\n");
	std::lock_guard lock(g_nfpMutex);
	nfp_data.nfpIsInitialized = true;
	nfp_data.isDetecting = false;
	nfp_data.isMounted = false;
	nfp_data.isReadOnly = false;
	nfp_data.hasActiveAmiibo = false;
	nfp_data.hasOpenApplicationArea = false;
	nfp_data.activateEvent = MPTR_NULL;
	nfp_data.deactivateEvent = MPTR_NULL;
	osLib_returnFromFunction(hCPU, 0);
}

void nnNfpExport_Finalize(PPCInterpreter_t* hCPU)
{
	std::lock_guard lock(g_nfpMutex);
	nfp_data.nfpIsInitialized = false;
	nfp_data.isDetecting = false;
	nfp_data.isMounted = false;
	nfp_data.hasActiveAmiibo = false;
	nfp_data.hasOpenApplicationArea = false;
	nfp_data.activateEvent = MPTR_NULL;
	nfp_data.deactivateEvent = MPTR_NULL;
	osLib_returnFromFunction(hCPU, 0);
}

void nnNfpExport_StartDetection(PPCInterpreter_t* hCPU)
{
	if (nnNfpIsSystemSettingsTitle())
		cemuLog_log(LogType::Force, "sys settings probe: nn_nfp StartDetection()");
	cemuLog_log(LogType::NN_NFP, "StartDetection()");
	nnNfpLock();
	nfp_data.isDetecting = true;
	nnNfpUnlock();
	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_StopDetection(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "StopDetection()");
	nnNfpLock();
	nfp_data.isDetecting = false;
	nnNfpUnlock();
	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

#define NFP_TAG_MAX_LENGTH	(10)

typedef struct  
{
	/* +0x00 */ uint8 uidLength;
	/* +0x01 */ uint8 uid[0xA];
	/* +0x0B */ uint8 unused0B[0x15];
	/* +0x20 */ uint8 technology;
	/* +0x21 */ uint8 tagType;
	/* +0x22 */ uint8 reserved[0x32];
}nfpTagInfo_t;

static_assert(sizeof(nfpTagInfo_t) == 0x54, "nfpTagInfo_t has invalid size");

void nnNfpExport_GetTagInfo(PPCInterpreter_t* hCPU)
{
	if (nnNfpIsSystemSettingsTitle())
		cemuLog_log(LogType::Force, "sys settings probe: nn_nfp GetTagInfo(0x{:08x})", hCPU->gpr[3]);
	cemuLog_log(LogType::NN_NFP, "GetTagInfo(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamStructPtr(tagInfo, nfpTagInfo_t, 0);

	nnNfpLock();
	if (nfp_data.hasActiveAmiibo == false)
	{
		nnNfpUnlock();
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0)); // todo: Return correct error code
		return;
	}
	memset(tagInfo, 0x00, sizeof(nfpTagInfo_t));

	memcpy(tagInfo->uid, nfp_data.amiiboProcessedData.uid, nfp_data.amiiboProcessedData.uidLength);
	tagInfo->uidLength = (uint8)nfp_data.amiiboProcessedData.uidLength;
	tagInfo->technology = 0; // NFC-A
	tagInfo->tagType = 2; // Type 2 (NTAG215)

	nnNfpUnlock();
	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_Mount(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "Mount()");
	nnNfpLock();
	if (nfp_data.hasActiveAmiibo == false)
	{
		nnNfpUnlock();
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0)); // todo: Return correct error code
		return;
	}
	nfp_data.isMounted = true;
	nfp_data.isReadOnly = false;
	nfp_data.isDetecting = false;
	nnNfpUnlock();
	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_Unmount(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "Unmount()");
	nfp_data.hasOpenApplicationArea = false;
	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_MountRom(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "MountRom()");
	nnNfpLock();
	if (nfp_data.hasActiveAmiibo == false)
	{
		nnNfpUnlock();
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0)); // todo: Return correct error code
		return;
	}
	nfp_data.isMounted = true;
	nfp_data.isReadOnly = true;
	nfp_data.isDetecting = false;
	nnNfpUnlock();
	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

namespace nn::nfp
{
	struct RomInfo
	{
		/* +0x00 */ uint8 characterId[3];
		/* +0x03 */ uint8 amiiboSeries;
		/* +0x04 */ uint16be number;
		/* +0x06 */ uint8 nfpType;
		/* +0x07 */ uint8 unused[0x2F];
	};

	static_assert(offsetof(RomInfo, amiiboSeries) == 0x3);
	static_assert(offsetof(RomInfo, number) == 0x4);
	static_assert(offsetof(RomInfo, nfpType) == 0x6);
	static_assert(sizeof(RomInfo) == 0x36);

	using ReadOnlyInfo = RomInfo; // same layout

	void GetRomInfo(RomInfo* romInfo)
	{
		cemu_assert_debug(nfp_data.hasActiveAmiibo);
		memset(romInfo, 0x00, sizeof(RomInfo));
		romInfo->characterId[0] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.gameAndCharacterId[0];
		romInfo->characterId[1] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.gameAndCharacterId[1];
		romInfo->characterId[2] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.characterVariation; // guessed
		romInfo->amiiboSeries = nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboSeries; // guessed
		romInfo->number = *(uint16be*)nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboModelNumber; // guessed
		romInfo->nfpType = nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboFigureType; // guessed
		memset(romInfo->unused, 0x00, sizeof(romInfo->unused));
	}

	nnResult GetNfpRomInfo(RomInfo* romInfo)
	{
		nnNfpLock();
		if (nfp_data.hasActiveAmiibo == false)
		{
			nnNfpUnlock();
			return BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0); // todo: Return correct error code
		}
		GetRomInfo(romInfo);
		nnNfpUnlock();
		return BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0);
	}

	nnResult GetNfpReadOnlyInfo(ReadOnlyInfo* readOnlyInfo)
	{
		nnNfpLock();
		if (nfp_data.hasActiveAmiibo == false)
		{
			nnNfpUnlock();
			return BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0); // todo: Return correct error code
		}
		GetRomInfo(readOnlyInfo);
		nnNfpUnlock();
		return BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0);
	}
};

typedef struct  
{
	uint16be year;
	uint8 month;
	uint8 day;
}nfpDate_t;

static uint16 nnNfpEncodeAmiiboDate(const nfpDate_t& date)
{
	uint32 yearOffset = 0;
	if (date.year >= 2000)
		yearOffset = (uint16)date.year - 2000;

	uint16 encodedDate = 0;
	encodedDate |= (uint16)(date.day & 0x1F);
	encodedDate |= (uint16)((date.month & 0x0F) << 5);
	encodedDate |= (uint16)((yearOffset & 0x7F) << 9);
	return encodedDate;
}

static nfpDate_t nnNfpDecodeAmiiboDate(uint16 encodedDate)
{
	nfpDate_t date{};
	date.day = (uint8)(encodedDate & 0x1F);
	date.month = (uint8)((encodedDate >> 5) & 0x0F);
	date.year = (uint16)(2000 + ((encodedDate >> 9) & 0x7F));
	return date;
}

static nfpDate_t nnNfpGetCurrentDate()
{
	coreinit::OSCalendarTime_t currentTime{};
	coreinit::OSTicksToCalendarTime(coreinit::OSGetTime(), &currentTime);

	nfpDate_t date{};
	date.year = (uint16)currentTime.year;
	date.month = (uint8)(currentTime.month + 1);
	date.day = (uint8)currentTime.dayOfMonth;
	return date;
}

typedef struct  
{
	/* +0x00 */ nfpDate_t date;
	/* +0x04 */ uint8 writeCount[2];
	/* +0x06 */ uint8 characterId[3];
	/* +0x09 */ uint8 amiiboSeries;
	/* +0x0A */ uint16be number;
	/* +0x0C */ uint8 nfpType;
	/* +0x0D */ uint8 nfpVersion;
	/* +0x0E */ uint16be applicationAreaSize;
	/* +0x10 */ uint8 unused[0x30];
}nfpCommonData_t;

static_assert(sizeof(nfpCommonData_t) == 0x40, "nfpCommonData_t has invalid size");
static_assert(offsetof(nfpCommonData_t, writeCount) == 0x4, "nfpCommonData.writeCount has invalid offset");
static_assert(offsetof(nfpCommonData_t, amiiboSeries) == 0x9, "nfpCommonData.seriesId has invalid offset");
static_assert(offsetof(nfpCommonData_t, nfpType) == 0xC, "nfpCommonData.nfpType has invalid offset");
static_assert(offsetof(nfpCommonData_t, applicationAreaSize) == 0xE, "nfpCommonData.applicationAreaSize has invalid offset");

void nnNfpExport_GetNfpCommonInfo(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "GetNfpCommonInfo(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamStructPtr(commonInfo, nfpCommonData_t, 0);

	nnNfpLock();
	if (nfp_data.hasActiveAmiibo == false)
	{
		nnNfpUnlock();
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0)); // todo: Return correct error code
		return;
	}
	// tag info format is currently unknown, so we just set it to all zeros for now
	if (sizeof(nfpCommonData_t) != 0x40 )
		assert_dbg();
	memset(commonInfo, 0x00, sizeof(nfpCommonData_t));

	cemuLog_logDebug(LogType::Force, "GetNfpCommonInfo(0x{:08x})");

	commonInfo->writeCount[0] = nfp_data.amiiboNFCData.writeCounter[0];
	commonInfo->writeCount[1] = nfp_data.amiiboNFCData.writeCounter[1];

	commonInfo->characterId[0] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.gameAndCharacterId[0];
	commonInfo->characterId[1] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.gameAndCharacterId[1];
	commonInfo->characterId[2] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.characterVariation;

	commonInfo->number = *(uint16be*)nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboModelNumber;
	commonInfo->amiiboSeries = nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboSeries;
	commonInfo->nfpType = nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboFigureType; // guessed

	commonInfo->applicationAreaSize = sizeof(nfp_data.amiiboInternal.applicationData); // not 100% sure if this is always set to the maximum

	nnNfpUnlock();
	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

typedef struct  
{
	/* +0x00 */ uint8 ownerMii[0x60]; 
	/* +0x60 */ uint8 nickname[0x16];
	/* +0x76 */ uint8 fontRegion;
	/* +0x77 */ uint8 country;
	/* +0x78 */ nfpDate_t registerDate;
	/* +0x7C */ uint8 reserved[0x2C];
}nfpRegisterInfo_t;

static_assert(sizeof(nfpRegisterInfo_t) == 0xA8);

typedef struct  
{
	/* +0x00 */ uint8 ownerMii[0x60];
	/* +0x60 */ uint8 nickname[0x16];
	/* +0x76 */ uint8 fontRegion;
	/* +0x77 */ uint8 reserved[0x2D];
}nfpRegisterInfoSet_t;

static_assert(sizeof(nfpRegisterInfoSet_t) == 0xA4);
static_assert(offsetof(nfpRegisterInfoSet_t, fontRegion) == 0x76);

static bool nnNfpIsRegisterInfoEmpty(const nfpRegisterInfoSet_t* registerInfoSet)
{
	for (size_t i = 0; i < sizeof(registerInfoSet->ownerMii); i++)
	{
		if (registerInfoSet->ownerMii[i] != 0)
			return false;
	}
	for (size_t i = 0; i < sizeof(registerInfoSet->nickname); i++)
	{
		if (registerInfoSet->nickname[i] != 0)
			return false;
	}
	return true;
}

void nnNfpExport_GetNfpRegisterInfo(PPCInterpreter_t* hCPU)
{
	if (nnNfpIsSystemSettingsTitle())
		cemuLog_log(LogType::Force, "sys settings probe: nn_nfp GetNfpRegisterInfo(0x{:08x})", hCPU->gpr[3]);
	cemuLog_log(LogType::NN_NFP, "GetNfpRegisterInfo(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamStructPtr(registerInfo, nfpRegisterInfo_t, 0);

	if(!registerInfo)
	{
		osLib_returnFromFunction(hCPU, 0xC1B03780);
		return;
	}

	memset(registerInfo, 0, sizeof(nfpRegisterInfo_t));
	std::lock_guard lock(g_nfpMutex);
	if (!nfp_data.hasActiveAmiibo || !nfp_data.isMounted)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT_STATUS(NN_RESULT_MODULE_NN_NFP, 0x6400));
		return;
	}
	if ((nfp_data.amiiboInternal.amiiboSettings.flags & 0x10) != 0)
	{
		memcpy(registerInfo->ownerMii, nfp_data.amiiboInternal.amiiboSettings.mii, sizeof(nfp_data.amiiboInternal.amiiboSettings.mii));
		memcpy(registerInfo->nickname, nfp_data.amiiboInternal.amiiboSettings.nickname, sizeof(nfp_data.amiiboInternal.amiiboSettings.nickname));
		registerInfo->fontRegion = nfp_data.amiiboInternal.amiiboSettings.flags & 0x0F;
		registerInfo->country = nfp_data.amiiboInternal.amiiboSettings.countryCode;
		registerInfo->registerDate = nnNfpDecodeAmiiboDate(nfp_data.amiiboInternal.amiiboSettings.date1);
	}
	else
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT_STATUS(NN_RESULT_MODULE_NN_NFP, 0x10900));
		return;
	}

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_InitializeRegisterInfoSet(PPCInterpreter_t* hCPU)
{
	if (nnNfpIsSystemSettingsTitle())
		cemuLog_log(LogType::Force, "sys settings probe: nn_nfp InitializeRegisterInfoSet(0x{:08x})", hCPU->gpr[3]);
	cemuLog_log(LogType::NN_NFP, "InitializeRegisterInfoSet(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamStructPtr(registerInfoSet, nfpRegisterInfoSet_t, 0);
	if (!registerInfoSet)
	{
		osLib_returnFromFunction(hCPU, 0xC1B03780);
		return;
	}

	memset(registerInfoSet, 0, sizeof(nfpRegisterInfoSet_t));

	const auto& currentAccount = Account::GetCurrentAccount();
	memcpy(registerInfoSet->ownerMii, currentAccount.GetMiiData().data(), currentAccount.GetMiiData().size());

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_SetNfpRegisterInfo(PPCInterpreter_t* hCPU)
{
	if (nnNfpIsSystemSettingsTitle())
		cemuLog_log(LogType::Force, "sys settings probe: nn_nfp SetNfpRegisterInfo(0x{:08x})", hCPU->gpr[3]);
	cemuLog_log(LogType::NN_NFP, "SetNfpRegisterInfo(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamStructPtr(registerInfoSet, nfpRegisterInfoSet_t, 0);
	if (!registerInfoSet)
	{
		osLib_returnFromFunction(hCPU, 0xC1B03780);
		return;
	}
	std::lock_guard lock(g_nfpMutex);
	if (!nfp_data.hasActiveAmiibo || !nfp_data.isMounted || nfp_data.isReadOnly)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT_STATUS(NN_RESULT_MODULE_NN_NFP, 0x6400));
		return;
	}
	auto previous = nfp_data.amiiboInternal;

	if (nnNfpIsRegisterInfoEmpty(registerInfoSet))
	{
		const auto& currentAccount = Account::GetCurrentAccount();
		memcpy(registerInfoSet->ownerMii, currentAccount.GetMiiData().data(), currentAccount.GetMiiData().size());
	}

	memcpy(nfp_data.amiiboInternal.amiiboSettings.mii, registerInfoSet->ownerMii, sizeof(nfp_data.amiiboInternal.amiiboSettings.mii));
	memcpy(nfp_data.amiiboInternal.amiiboSettings.nickname, registerInfoSet->nickname, sizeof(nfp_data.amiiboInternal.amiiboSettings.nickname));
	nfpDate_t currentDate = nnNfpGetCurrentDate();
	uint16 encodedDate = nnNfpEncodeAmiiboDate(currentDate);
	nfp_data.amiiboInternal.amiiboSettings.date1 = encodedDate;
	nfp_data.amiiboInternal.amiiboSettings.date2 = encodedDate;
	nfp_data.amiiboInternal.amiiboSettings.countryCode = (uint8)Account::GetCurrentAccount().GetCountry();
	nfp_data.amiiboInternal.amiiboSettings.flags |= 0x10; // set registered bit
	nfp_data.amiiboInternal.amiiboSettings.flags =
		(nfp_data.amiiboInternal.amiiboSettings.flags & 0xF0) | (registerInfoSet->fontRegion & 0x0F);

	if (!nnNfp_writeCurrentAmiibo())
	{
		nfp_data.amiiboInternal = previous;
		cemuLog_log(LogType::Force, "Failed to write Amiibo data after SetNfpRegisterInfo");
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

struct nfpAdminInfo_t
{
	uint64be titleId;
	uint32be accessId;
	uint16be applicationAreaWrites;
	uint8 flags;
	uint8 formatVersion;
	uint8 platform;
	uint8 reserved[0x2F];
};

static_assert(sizeof(nfpAdminInfo_t) == 0x40);
static_assert(offsetof(nfpAdminInfo_t, flags) == 0xE);

void nnNfpExport_GetNfpAdminInfo(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(info, nfpAdminInfo_t, 0);
	if (!info)
	{
		osLib_returnFromFunction(hCPU, 0xC1B03780);
		return;
	}
	std::lock_guard lock(g_nfpMutex);
	if (!nfp_data.hasActiveAmiibo || !nfp_data.isMounted)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT_STATUS(NN_RESULT_MODULE_NN_NFP, 0x6400));
		return;
	}
	memset(info, 0, sizeof(*info));
	const auto& settings = nfp_data.amiiboInternal.amiiboSettings;
	info->titleId = ((uint64)(uint32)settings.appDataTitleIdHigh << 32) | (uint32)settings.appDataTitleIdLow;
	info->accessId = nfp_data.amiiboInternal.amiiboSettings.getAppDataAppId();
	info->applicationAreaWrites = settings.appWriteCounter;
	info->flags = (settings.flags >> 4) & 3;
	info->formatVersion = nfp_data.amiiboInternal.amiiboIdentificationBlock.ukn_02;
	info->platform = (settings.flags & 0x20) ? 1 : 0;
	osLib_returnFromFunction(hCPU, 0);
}

void nnNfpExport_IsExistApplicationArea(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "IsExistApplicationArea()");
	if (!nfp_data.hasActiveAmiibo || !nfp_data.isMounted)
	{
		osLib_returnFromFunction(hCPU, 0);
		return;
	}
	bool appAreaExists = (nfp_data.amiiboInternal.amiiboSettings.flags & 0x20) != 0;
	osLib_returnFromFunction(hCPU, appAreaExists ? 1 : 0);
}

void nnNfpExport_OpenApplicationArea(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "OpenApplicationArea(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamU32(appAreaId, 0);

	// note - this API doesn't fail if the application area has already been opened?

	if (!(nfp_data.amiiboInternal.amiiboSettings.flags & 0x20))
	{
		// no application data set
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, NN_RESULT_NFP_CODE_NOAPPAREA));
		return;
	}

	uint32 nfpAppAreaId = nfp_data.amiiboInternal.amiiboSettings.getAppDataAppId();
	if (nfpAppAreaId != appAreaId)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, NN_RESULT_NFP_CODE_APPAREAIDMISMATCH));
		return;
	}
	nfp_data.hasOpenApplicationArea = true;

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_ReadApplicationArea(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "ReadApplicationArea(0x{:08x}, 0x{:x})", hCPU->gpr[3], hCPU->gpr[4]);
	ppcDefineParamPtr(bufferPtr, uint8*, 0);
	ppcDefineParamU32(len, 1);

	if (!nfp_data.hasOpenApplicationArea)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}
	len = std::min(len, (uint32)sizeof(nfp_data.amiiboInternal.applicationData));
	memcpy(bufferPtr, nfp_data.amiiboInternal.applicationData, len);

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_WriteApplicationArea(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "WriteApplicationArea(0x{:08x}, 0x{:x}, 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
	ppcDefineParamPtr(bufferPtr, uint8*, 0);
	ppcDefineParamU32(len, 1);
	
	if (!nfp_data.hasOpenApplicationArea)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}
	if (nfp_data.isReadOnly)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	len = std::min(len, (uint32)sizeof(nfp_data.amiiboInternal.applicationData));
	memcpy(nfp_data.amiiboInternal.applicationData, bufferPtr, len);
	// remaining data is filled with random bytes
	for (uint32 i = len; i < sizeof(nfp_data.amiiboInternal.applicationData); i++)
		nfp_data.amiiboInternal.applicationData[i] = rand() & 0xFF;

	nfp_data.amiiboInternal.amiiboSettings.appWriteCounter = nfp_data.amiiboInternal.amiiboSettings.appWriteCounter + 1;

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

typedef struct  
{
	uint32be appAreaId;
	MEMPTR<uint8> initialData;
	uint32be initialLen;
	// more?
}NfpCreateInfo_t;

void nnNfpExport_CreateApplicationArea(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "CreateApplicationArea(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamPtr(createInfo, NfpCreateInfo_t, 0);

	if (nfp_data.hasOpenApplicationArea || (nfp_data.amiiboInternal.amiiboSettings.flags&0x20))
	{
		// cant create app area if it already exists
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}
	if (nfp_data.isReadOnly)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	void* writePtr = createInfo->initialData.GetPtr();
	uint32 writeSize = createInfo->initialLen;
	if (writeSize > sizeof(nfp_data.amiiboInternal.applicationData))
	{
		// requested write size larger than available space
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	nfp_data.amiiboInternal.amiiboSettings.setAppDataAppId(createInfo->appAreaId);
	nfp_data.amiiboInternal.amiiboSettings.flags |= 0x20; // set application data exists bit
	nfp_data.amiiboInternal.amiiboSettings.appWriteCounter = nfp_data.amiiboInternal.amiiboSettings.appWriteCounter + 1;

	nfp_data.hasOpenApplicationArea = false;

	// write initial data to application area
	memcpy(nfp_data.amiiboInternal.applicationData, writePtr, writeSize);
	// remaining data is filled with random bytes
	for (uint32 i = writeSize; i < sizeof(nfp_data.amiiboInternal.applicationData); i++)
		nfp_data.amiiboInternal.applicationData[i] = rand() & 0xFF;

	// this API forces a flush (unsure, but without this data written by Smash doesn't stick)
	if (!nnNfp_writeCurrentAmiibo())
	{
		cemuLog_log(LogType::Force, "Failed to write Amiibo file data when trying to remove appArea");
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_DeleteApplicationArea(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "DeleteApplicationArea()");

	if (nfp_data.isReadOnly)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}
	if ((nfp_data.amiiboInternal.amiiboSettings.flags & 0x20) == 0)
	{
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	nfp_data.amiiboInternal.amiiboSettings.setAppDataAppId(0);
	nfp_data.amiiboInternal.amiiboSettings.flags &= ~0x20;
	nfp_data.amiiboInternal.amiiboSettings.appWriteCounter = nfp_data.amiiboInternal.amiiboSettings.appWriteCounter + 1;

	// this API forces a flush
	if (!nnNfp_writeCurrentAmiibo())
	{
		cemuLog_log(LogType::Force, "Failed to write Amiibo file data when trying to remove appArea");
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

void nnNfpExport_Flush(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "Flush()");

	// write Amiibo data
	if (nfp_data.isReadOnly) 
	{
		cemuLog_log(LogType::Force, "Cannot write to Amiibo when it is mounted in read-only mode");
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	if (!nnNfp_writeCurrentAmiibo())
	{
		cemuLog_log(LogType::Force, "Failed to write Amiibo data");
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0));
		return;
	}

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

typedef struct  
{
	/* +0x000 */ uint8 ukn00[0x10];
	/* +0x010 */ uint32be mode;
	/* +0x014 */ uint8 tagInfo[0x54];
	/* +0x068 */ uint8 isRegistered;
	/* +0x069 */ uint8 padding[3];
	/* +0x06C */ uint8 registerInfo[0xA8];
	/* +0x114 */ uint8 commonInfo[0x40];
	/* +0x154 */ uint8 ukn154[0x20];
}AmiiboSettingsArgs_t;

static_assert(sizeof(AmiiboSettingsArgs_t) == 0x174);
static_assert(offsetof(AmiiboSettingsArgs_t, mode) == 0x10);
static_assert(offsetof(AmiiboSettingsArgs_t, tagInfo) == 0x14);
static_assert(offsetof(AmiiboSettingsArgs_t, isRegistered) == 0x68);
static_assert(offsetof(AmiiboSettingsArgs_t, registerInfo) == 0x6C);
static_assert(offsetof(AmiiboSettingsArgs_t, commonInfo) == 0x114);

static struct
{
	uint64 callerTitle{};
	std::array<uint8, 0x164> args{};
	uint64 shortcutTitle{};
	std::function<void()> shortcutReturn;
}s_amiiboSettingsLaunch;

bool nnNfp_prepareRegistrationShortcut(uint64 titleId, std::function<void()> onReturn)
{
	if (!nnNfp_isAmiiboSettingsTitle(titleId) || !onReturn)
		return false;
	std::lock_guard lock(g_nfpMutex);
	s_amiiboSettingsLaunch = {};
	s_amiiboSettingsLaunch.shortcutTitle = titleId;
	s_amiiboSettingsLaunch.shortcutReturn = std::move(onReturn);
	const uint32be mode = 0x64; // enter the verified native amiibo menu
	memcpy(s_amiiboSettingsLaunch.args.data(), &mode, sizeof(mode));
	return true;
}

void nnNfp_cancelRegistrationShortcut()
{
	std::lock_guard lock(g_nfpMutex);
	if (s_amiiboSettingsLaunch.shortcutTitle)
		s_amiiboSettingsLaunch = {};
}

void nnNfpExport_InitializeAmiiboSettingsArgsIn(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(args, uint8, 0);
	if (!args)
	{
		osLib_returnFromFunction(hCPU, 0xC1B03780);
		return;
	}
	memset(args, 0, 0x164);
	osLib_returnFromFunction(hCPU, 0);
}

void nnNfpExport_SwitchToAmiiboSettings(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(args, uint8, 0);
	const auto caller = CafeSystem::GetForegroundTitleId();
	const auto settingsTitle = _SYSGetSystemApplicationTitleId(1);
	const auto cabinetTitle = 0x000500301006D00Aull + (settingsTitle & 0xF00);
	// Only Settings can currently be relaunched safely; a game's guest context
	// must not be discarded by treating an applet switch as a normal title launch.
	if (!args || caller != settingsTitle || !CafeTitleList::GetGameInfo(cabinetTitle).IsValid())
	{
		cemuLog_log(LogType::Force, "amiibo settings: cannot switch from {:016x} to {:016x}", caller, cabinetTitle);
		osLib_returnFromFunction(hCPU, BUILD_NN_RESULT_STATUS(NN_RESULT_MODULE_NN_NFP, 0x6400));
		return;
	}
	{
		std::lock_guard lock(g_nfpMutex);
		s_amiiboSettingsLaunch = {};
		s_amiiboSettingsLaunch.callerTitle = caller;
		memcpy(s_amiiboSettingsLaunch.args.data(), args, s_amiiboSettingsLaunch.args.size());
	}
	cemuLog_log(LogType::Force, "amiibo settings: native Settings launch {:016x}", cabinetTitle);
	osLib_returnFromFunction(hCPU, coreinit::__LaunchByTitleId(cabinetTitle, 0, nullptr));
}

void nnNfpExport_ReturnToCallerWithAmiiboSettingsResult(PPCInterpreter_t* hCPU)
{
	ppcDefineParamStructPtr(result, uint32be, 0);
	if (!result)
	{
		osLib_returnFromFunction(hCPU, 0xC1B03780);
		return;
	}
	uint64 caller;
	std::function<void()> shortcutReturn;
	{
		std::lock_guard lock(g_nfpMutex);
		if (s_amiiboSettingsLaunch.shortcutTitle == CafeSystem::GetForegroundTitleId() &&
			s_amiiboSettingsLaunch.shortcutReturn)
		{
			shortcutReturn = std::move(s_amiiboSettingsLaunch.shortcutReturn);
			s_amiiboSettingsLaunch = {};
		}
		caller = s_amiiboSettingsLaunch.callerTitle;
		if (!shortcutReturn && (caller != _SYSGetSystemApplicationTitleId(1) ||
			CafeSystem::GetForegroundTitleId() != 0x000500301006D00Aull + (caller & 0xF00)))
		{
			osLib_returnFromFunction(hCPU, BUILD_NN_RESULT_STATUS(NN_RESULT_MODULE_NN_NFP, 0x6400));
			return;
		}
		s_amiiboSettingsLaunch = {};
	}
	if (shortcutReturn)
	{
		cemuLog_log(LogType::Force, "amiibo settings: native shortcut result {:08x}, returning to Cemu game list", (uint32)*result);
		shortcutReturn();
		osLib_returnFromFunction(hCPU, 0);
		return;
	}
	cemuLog_log(LogType::Force, "amiibo settings: native result {:08x}, returning to Settings", (uint32)*result);
	osLib_returnFromFunction(hCPU, coreinit::__LaunchByTitleId(caller, 0, nullptr));
}

void nnNfpExport_GetAmiiboSettingsArgs(PPCInterpreter_t* hCPU)
{
	if (nnNfpIsSystemSettingsTitle())
		cemuLog_log(LogType::Force, "sys settings probe: nn_nfp GetAmiiboSettingsArgs(0x{:08x})", hCPU->gpr[3]);
	cemuLog_log(LogType::NN_NFP, "GetAmiiboSettingsArgs(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamStructPtr(settingsArg, AmiiboSettingsArgs_t, 0);
	if (!settingsArg)
	{
		osLib_returnFromFunction(hCPU, 0xC1B03780);
		return;
	}

	memset(settingsArg, 0, sizeof(AmiiboSettingsArgs_t));
	std::lock_guard lock(g_nfpMutex);
	if ((s_amiiboSettingsLaunch.callerTitle &&
		CafeSystem::GetForegroundTitleId() == 0x000500301006D00Aull + (s_amiiboSettingsLaunch.callerTitle & 0xF00)) ||
		(s_amiiboSettingsLaunch.shortcutTitle && s_amiiboSettingsLaunch.shortcutTitle == CafeSystem::GetForegroundTitleId()))
	{
		memcpy(&settingsArg->mode, s_amiiboSettingsLaunch.args.data(), s_amiiboSettingsLaunch.args.size());
		cemuLog_log(LogType::Force, "amiibo settings: native arguments mode {}", (uint32)settingsArg->mode);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	// modes:
	// 0 -> Register owner and nickname
	// 0x64 -> Launch normally

	settingsArg->mode = 0x64; // no caller arguments: normal native amiibo menu
	settingsArg->isRegistered = nfp_data.hasActiveAmiibo && (nfp_data.amiiboInternal.amiiboSettings.flags & 0x10) ? 1u : 0u;

	if (nfp_data.hasActiveAmiibo)
	{
		nfpTagInfo_t* tagInfo = reinterpret_cast<nfpTagInfo_t*>(settingsArg->tagInfo);
		memset(tagInfo, 0, sizeof(nfpTagInfo_t));
		memcpy(tagInfo->uid, nfp_data.amiiboProcessedData.uid, nfp_data.amiiboProcessedData.uidLength);
		tagInfo->uidLength = (uint8)nfp_data.amiiboProcessedData.uidLength;
		tagInfo->tagType = 2;

		nfpRegisterInfo_t* registerInfo = reinterpret_cast<nfpRegisterInfo_t*>(settingsArg->registerInfo);
		memset(registerInfo, 0, sizeof(nfpRegisterInfo_t));
		if ((nfp_data.amiiboInternal.amiiboSettings.flags & 0x10) != 0)
		{
			memcpy(registerInfo->ownerMii, nfp_data.amiiboInternal.amiiboSettings.mii, sizeof(nfp_data.amiiboInternal.amiiboSettings.mii));
			memcpy(registerInfo->nickname, nfp_data.amiiboInternal.amiiboSettings.nickname, sizeof(nfp_data.amiiboInternal.amiiboSettings.nickname));
			registerInfo->fontRegion = nfp_data.amiiboInternal.amiiboSettings.flags & 0x0F;
			registerInfo->country = nfp_data.amiiboInternal.amiiboSettings.countryCode;
			registerInfo->registerDate = nnNfpDecodeAmiiboDate(nfp_data.amiiboInternal.amiiboSettings.date1);
		}

		nfpCommonData_t* commonInfo = reinterpret_cast<nfpCommonData_t*>(settingsArg->commonInfo);
		memset(commonInfo, 0, sizeof(nfpCommonData_t));
		commonInfo->date = nnNfpDecodeAmiiboDate(nfp_data.amiiboInternal.amiiboSettings.date2);
		commonInfo->writeCount[0] = nfp_data.amiiboNFCData.writeCounter[0];
		commonInfo->writeCount[1] = nfp_data.amiiboNFCData.writeCounter[1];
		commonInfo->characterId[0] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.gameAndCharacterId[0];
		commonInfo->characterId[1] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.gameAndCharacterId[1];
		commonInfo->characterId[2] = nfp_data.amiiboNFCData.amiiboIdentificationBlock.characterVariation;
		commonInfo->number = *(uint16be*)nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboModelNumber;
		commonInfo->amiiboSeries = nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboSeries;
		commonInfo->nfpType = nfp_data.amiiboNFCData.amiiboIdentificationBlock.amiiboFigureType;
		commonInfo->nfpVersion = 0;
		commonInfo->applicationAreaSize = sizeof(nfp_data.amiiboInternal.applicationData);
	}

	osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_SUCCESS, NN_RESULT_MODULE_NN_NFP, 0));
}

static void nnNfp_unloadAmiiboLocked()
{
	nfp_data.isMounted = false;
	nfp_data.hasActiveAmiibo = false;
	nfp_data.amiiboProcessedData.uidLength = 0;
	memset(nfp_data.amiiboProcessedData.uid, 0, sizeof(nfp_data.amiiboProcessedData.uid));
}

static void nnNfp_signalDeactivateEventLocked()
{
	if (!nfp_data.deactivateEvent)
		return;

	coreinit::OSEvent* osEvent = (coreinit::OSEvent*)memory_getPointerFromVirtualOffset(nfp_data.deactivateEvent);
	coreinit::OSSignalEvent(osEvent);
}

void nnNfp_unloadAmiibo()
{
	nnNfpLock();
	nnNfp_unloadAmiiboLocked();
	nnNfpUnlock();
}

bool nnNfp_isInitialized()
{
	return nfp_data.nfpIsInitialized;
}

// CEMU NFC error codes
#define NFC_ERROR_NONE					(0)
#define NFC_ERROR_NO_ACCESS				(1)
#define NFC_ERROR_INVALID_FILE_FORMAT	(2)

bool nnNfp_touchNfcTagFromFile(const fs::path& filePath, uint32* nfcError)
{
	AmiiboRawNFCData rawData = { 0 };
	auto nfcData = FileStream::LoadIntoMemory(filePath);
	if (!nfcData)
	{
		*nfcError = NFC_ERROR_NO_ACCESS;
		return false;
	}
	if (nfcData->size() < sizeof(AmiiboRawNFCData))
	{
		*nfcError = NFC_ERROR_INVALID_FILE_FORMAT;
		return false;
	}
	memcpy(&rawData, nfcData->data(), sizeof(AmiiboRawNFCData));

	// verify if the file is a valid ntag215/amiibo file
	if (rawData.dynamicLock[0] != 0x01 || rawData.dynamicLock[1] != 0x00 || rawData.dynamicLock[2] != 0x0F || rawData.dynamicLock[3] != 0xBD)
	{
		// Temporary workaround to fix corrupted files by old cemu versions
		rawData.dynamicLock[0] = 0x01;
		rawData.dynamicLock[1] = 0x00;
		rawData.dynamicLock[2] = 0x0F;
		rawData.dynamicLock[3] = 0xBD;

		// *nfcError = NFC_ERROR_INVALID_FILE_FORMAT;
		// return false;
	}
	if (rawData.cfg0[0] != 0x00 || rawData.cfg0[1] != 0x00 || rawData.cfg0[2] != 0x00 || rawData.cfg0[3] != 0x04)
	{
		*nfcError = NFC_ERROR_INVALID_FILE_FORMAT;
		return false;
	}
	if (rawData.cfg1[0] != 0x5F || rawData.cfg1[1] != 0x00 || rawData.cfg1[2] != 0x00 || rawData.cfg1[3] != 0x00)
	{
		*nfcError = NFC_ERROR_INVALID_FILE_FORMAT;
		return false;
	}
	if (rawData.staticLock[0] != 0x0F || rawData.staticLock[1] != 0xE0)
	{
		*nfcError = NFC_ERROR_INVALID_FILE_FORMAT;
		return false;
	}
	if (rawData.cc[0] != 0xF1 || rawData.cc[1] != 0x10 || rawData.cc[2] != 0xFF || rawData.cc[3] != 0xEE)
	{
		*nfcError = NFC_ERROR_INVALID_FILE_FORMAT;
		return false;
	}

	// process uid
	uint8 serialNumber[7];
	serialNumber[0] = rawData.ntagSerial[0];
	serialNumber[1] = rawData.ntagSerial[1];
	serialNumber[2] = rawData.ntagSerial[2];
	serialNumber[3] = rawData.ntagSerial[4];
	serialNumber[4] = rawData.ntagSerial[5];
	serialNumber[5] = rawData.ntagSerial[6];
	serialNumber[6] = rawData.nintendoId;

	uint8 serialCheckByte0 = rawData.ntagSerial[3];
	uint8 serialCheckByte1 = rawData.lockBytes[0];

	uint8 bcc0 = serialNumber[0] ^ serialNumber[1] ^ serialNumber[2] ^ 0x88;
	uint8 bcc1 = serialNumber[3] ^ serialNumber[4] ^ serialNumber[5] ^ serialNumber[6];

	if (serialCheckByte0 != bcc0 || serialCheckByte1 != bcc1)
	{
		cemuLog_log(LogType::Force, "nn_nfp: Mismatch in serial checksum of scanned NFC tag");
	}
	nnNfpLock();
	if (nfp_data.hasActiveAmiibo)
	{
		nnNfp_unloadAmiiboLocked();
		nnNfp_signalDeactivateEventLocked();
	}
	nfp_data.amiiboProcessedData.uidLength = 7;
	memcpy(nfp_data.amiiboProcessedData.uid, serialNumber, 7);
	memcpy(&nfp_data.amiiboNFCData, &rawData, sizeof(AmiiboRawNFCData));
	// decrypt amiibo
	amiiboDecrypt();
	nfp_data.amiiboPath = filePath;
	nfp_data.hasActiveAmiibo = true;
	if (nfp_data.activateEvent)
	{
		MEMPTR<coreinit::OSEvent> osEvent(nfp_data.activateEvent);
		coreinit::OSSignalEvent(osEvent);
	}
	nfp_data.amiiboTouchTime = GetTickCount();
	nnNfpUnlock();
	*nfcError = NFC_ERROR_NO_ACCESS;
	return true;
}

bool nnNfp_writeCurrentAmiibo()
{
	nnNfpLock();
	if (!nfp_data.hasActiveAmiibo)
	{
		nnNfpUnlock();
		return false;
	}

	uint16 writeCounter = nfp_data.amiiboInternal.writeCounterLow + (nfp_data.amiiboInternal.writeCounterHigh << 8);
	writeCounter++;
	nfp_data.amiiboInternal.writeCounterLow = writeCounter & 0xFF;
	nfp_data.amiiboInternal.writeCounterHigh = (writeCounter >> 8) & 0xFF;

	// open file for writing
	FileStream* fs = FileStream::openFile2(nfp_data.amiiboPath, true);
	if (!fs)
	{
		nfp_data.amiiboInternal.writeCounterLow = (writeCounter - 1) & 0xFF;
		nfp_data.amiiboInternal.writeCounterHigh = ((writeCounter - 1) >> 8) & 0xFF;
		nnNfpUnlock();
		return false;
	}
	// encrypt Amiibo and convert to NFC format
	AmiiboRawNFCData nfcData;
	amiiboEncrypt(&nfcData);
	// write to file
	const bool written = fs->writeData(&nfcData, sizeof(AmiiboRawNFCData)) == sizeof(AmiiboRawNFCData);
	delete fs;
	if (written)
		nfp_data.amiiboNFCData = nfcData;
	else
	{
		nfp_data.amiiboInternal.writeCounterLow = (writeCounter - 1) & 0xFF;
		nfp_data.amiiboInternal.writeCounterHigh = ((writeCounter - 1) >> 8) & 0xFF;
	}

	nnNfpUnlock();
	return written;
}

void nnNfp_update()
{
	// lock-free check if amiibo is touching
	if (nfp_data.hasActiveAmiibo == false)
		return;
	if (!nnNfpTryLock())
		return;
	std::lock_guard lock(g_nfpMutex, std::adopt_lock);
	// make sure amiibo is still touching after acquiring lock
	if (nfp_data.hasActiveAmiibo == false)
		return;
	uint32 amiiboElapsedTouchTime = GetTickCount() - nfp_data.amiiboTouchTime;
	if (amiiboElapsedTouchTime >= 1500)
	{
		nnNfp_unloadAmiiboLocked();
		nnNfp_signalDeactivateEventLocked();
	}
}

void nnNfpExport_GetNfpState(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::NN_NFP, "GetNfpState()");

	// workaround for Mario Party 10 eating CPU cycles in an infinite loop (maybe due to incorrect NFP detection handling?)
	uint64 titleId = CafeSystem::GetForegroundTitleId();
	if (titleId == 0x0005000010162d00 || titleId == 0x0005000010162e00)
	{
		coreinit::OSSleepTicks(ESPRESSO_CORE_CLOCK / 100); // pause for 10ms
	}

	uint32 nfpState;
	if (nfp_data.nfpIsInitialized == false)
	{
		nfpState = NFP_STATE_NONE;
	}
	else
	{
		if (nfp_data.isMounted && nfp_data.hasActiveAmiibo)
		{
			if (nfp_data.isReadOnly)
				nfpState = NFP_STATE_RW_MOUNT_ROM;
			else
				nfpState = NFP_STATE_RW_MOUNT;
		}
		else if (nfp_data.isDetecting)
		{
			// todo: is this handled correctly?
			if (nfp_data.hasActiveAmiibo)
			{
				nfpState = NFP_STATE_RW_ACTIVE;
			}
			else
				nfpState = NFP_STATE_RW_SEARCH;
		}
		else
		{
			nfpState = NFP_STATE_INIT;
		}
	}
	// returns state of nfp library
	osLib_returnFromFunction(hCPU, nfpState);
}

namespace nn::nfp
{
	typedef struct  
	{
		/* +0x00 */ uint8 uidLength;
		/* +0x01 */ uint8 uid[0xA];
		/* +0x0B */ uint8 ukn0B;
		/* +0x0C */ uint8 ukn0C;
		/* +0x0D */ uint8 ukn0D;
		// more?
	}NFCTagInfoCallbackParam_t;

	uint32 NFCGetTagInfo(uint32 index, uint32 timeout, MPTR functionPtr, void* userParam)
	{
		cemuLog_log(LogType::NN_NFP, "NFCGetTagInfo({},{},0x{:08x},0x{:08x})", index, timeout, functionPtr, userParam ? memory_getVirtualOffsetFromPointer(userParam) : 0);

		cemu_assert(index == 0);

		nnNfpLock();

		if (!nfp_data.hasActiveAmiibo || nfp_data.amiiboProcessedData.uidLength == 0)
		{
			const uint32 noTagResult = BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_NN_NFP, 0);
			nnNfpUnlock();
			PPCCoreCallback(functionPtr, index, noTagResult, userParam);
			return noTagResult;
		}

		StackAllocator<NFCTagInfoCallbackParam_t> _callbackParam;
		NFCTagInfoCallbackParam_t* callbackParam = _callbackParam.GetPointer();

		memset(callbackParam, 0x00, sizeof(NFCTagInfoCallbackParam_t));

		memcpy(callbackParam->uid, nfp_data.amiiboProcessedData.uid, nfp_data.amiiboProcessedData.uidLength);
		callbackParam->uidLength = (uint8)nfp_data.amiiboProcessedData.uidLength;

		PPCCoreCallback(functionPtr, index, 0, _callbackParam.GetPointer(), userParam);

		nnNfpUnlock();


		return 0; // 0 -> success
	}

	uint32 GetErrorCode(uint32 result)
	{
		uint32 level = (result >> 0x1b) & 3;
		uint32 mask = 0x7f00000;
		if (level != 3) 
		{
			mask = 0x1ff00000;
		}
		if (((result & mask) == 0x1b00000) && ((result & 0x80000000) != 0)) 
		{
			mask = 0x3ff;
			if (level != 3) 
			{
				mask = 0xfffff;
			}
			return ((result & mask) >> 7) + 1680000;
		}
		return 1680000;
	}

	void nnNfp_load()
	{
		osLib_addFunction("nn_nfp", "AntennaCheck__Q2_2nn3nfpFv", nnNfpExport_Initialize);
		osLib_addFunction("nn_nfp", "Finalize__Q2_2nn3nfpFv", nnNfpExport_Finalize);
		osLib_addFunction("nn_nfp", "SetActivateEvent__Q2_2nn3nfpFP7OSEvent", nnNfpExport_SetActivateEvent);
		osLib_addFunction("nn_nfp", "SetDeactivateEvent__Q2_2nn3nfpFP7OSEvent", nnNfpExport_SetDeactivateEvent);
		osLib_addFunction("nn_nfp", "StartDetection__Q2_2nn3nfpFv", nnNfpExport_StartDetection);
		osLib_addFunction("nn_nfp", "StopDetection__Q2_2nn3nfpFv", nnNfpExport_StopDetection);

		osLib_addFunction("nn_nfp", "GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo", nnNfpExport_GetTagInfo);
		osLib_addFunction("nn_nfp", "Mount__Q2_2nn3nfpFv", nnNfpExport_Mount);
		osLib_addFunction("nn_nfp", "MountRom__Q2_2nn3nfpFv", nnNfpExport_MountRom);
		osLib_addFunction("nn_nfp", "Unmount__Q2_2nn3nfpFv", nnNfpExport_Unmount);
		osLib_addFunction("nn_nfp", "GetNfpCommonInfo__Q2_2nn3nfpFPQ3_2nn3nfp10CommonInfo", nnNfpExport_GetNfpCommonInfo);
		osLib_addFunction("nn_nfp", "GetNfpRegisterInfo__Q2_2nn3nfpFPQ3_2nn3nfp12RegisterInfo", nnNfpExport_GetNfpRegisterInfo);
		osLib_addFunction("nn_nfp", "GetNfpAdminInfo__Q2_2nn3nfpFPQ3_2nn3nfp9AdminInfo", nnNfpExport_GetNfpAdminInfo);

		osLib_addFunction("nn_nfp", "InitializeRegisterInfoSet__Q2_2nn3nfpFPQ3_2nn3nfp15RegisterInfoSet", nnNfpExport_InitializeRegisterInfoSet);
		osLib_addFunction("nn_nfp", "SetNfpRegisterInfo__Q2_2nn3nfpFRCQ3_2nn3nfp15RegisterInfoSet", nnNfpExport_SetNfpRegisterInfo);

		osLib_addFunction("nn_nfp", "IsExistApplicationArea__Q2_2nn3nfpFv", nnNfpExport_IsExistApplicationArea);
		osLib_addFunction("nn_nfp", "OpenApplicationArea__Q2_2nn3nfpFUi", nnNfpExport_OpenApplicationArea);
		osLib_addFunction("nn_nfp", "CreateApplicationArea__Q2_2nn3nfpFRCQ3_2nn3nfp25ApplicationAreaCreateInfo", nnNfpExport_CreateApplicationArea);
		osLib_addFunction("nn_nfp", "DeleteApplicationArea__Q2_2nn3nfpFv", nnNfpExport_DeleteApplicationArea);
		osLib_addFunction("nn_nfp", "ReadApplicationArea__Q2_2nn3nfpFPvUi", nnNfpExport_ReadApplicationArea);
		osLib_addFunction("nn_nfp", "WriteApplicationArea__Q2_2nn3nfpFPCvUiRCQ3_2nn3nfp5TagId", nnNfpExport_WriteApplicationArea);

		osLib_addFunction("nn_nfp", "Flush__Q2_2nn3nfpFv", nnNfpExport_Flush);

		osLib_addFunction("nn_nfp", "Initialize__Q2_2nn3nfpFv", nnNfpExport_Initialize);
		osLib_addFunction("nn_nfp", "GetNfpState__Q2_2nn3nfpFv", nnNfpExport_GetNfpState);

		osLib_addFunction("nn_nfp", "GetAmiiboSettingsArgs__Q2_2nn3nfpFPQ3_2nn3nfp18AmiiboSettingsArgs", nnNfpExport_GetAmiiboSettingsArgs);
		osLib_addFunction("nn_nfp", "InitializeAmiiboSettingsArgsIn__Q2_2nn3nfpFPQ3_2nn3nfp20AmiiboSettingsArgsIn", nnNfpExport_InitializeAmiiboSettingsArgsIn);
		osLib_addFunction("nn_nfp", "SwitchToAmiiboSettings__Q2_2nn3nfpFRCQ3_2nn3nfp20AmiiboSettingsArgsIn", nnNfpExport_SwitchToAmiiboSettings);
		osLib_addFunction("nn_nfp", "ReturnToCallerWithAmiiboSettingsResult__Q2_2nn3nfpFRCQ3_2nn3nfp20AmiiboSettingsResult", nnNfpExport_ReturnToCallerWithAmiiboSettingsResult);
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "nn_nfp";
		}

		void RPLMapped() override
		{
			nnNfp_load(); // legacy interface, update these to use cafeExportRegister / cafeExportRegisterFunc

			cafeExportRegisterFunc(nn::nfp::GetErrorCode, "nn_nfp", "GetErrorCode__Q2_2nn3nfpFRCQ2_2nn6Result", LogType::NN_NFP);
			cafeExportRegisterFunc(nn::nfp::GetNfpRomInfo, "nn_nfp", "GetNfpRomInfo__Q2_2nn3nfpFPQ3_2nn3nfp7RomInfo", LogType::NN_NFP);
			cafeExportRegisterFunc(nn::nfp::GetNfpReadOnlyInfo, "nn_nfp", "GetNfpReadOnlyInfo__Q2_2nn3nfpFPQ3_2nn3nfp12ReadOnlyInfo", LogType::NN_NFP);
		};

	}s_COSnnNfpModule;

	COSModule* GetModule()
	{
		return &s_COSnnNfpModule;
	}
}
