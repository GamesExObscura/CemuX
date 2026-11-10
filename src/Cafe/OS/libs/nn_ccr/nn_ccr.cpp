#include "Cafe/OS/common/OSCommon.h"
#include "nn_ccr.h"
#include "Cafe/OS/libs/coreinit/HomeSession.h"

namespace nn::ccr
{
	sint32 CCRSysCaffeineBootCheck()
	{
		cemuLog_logDebug(LogType::Force, "CCRSysCaffeineBootCheck()");
		return -1;
	}

	sint32 CCRSysGetRumbleMode(uint32be* mode)
	{
		bool enabled{};
		if (!mode || !coreinit::NativeHomeSession::GetGamePadRumble(enabled)) return -1;
		*mode = enabled ? 1 : 0;
		return 0;
	}

	sint32 CCRSysSetRumbleMode(uint32 mode)
	{
		if (mode > 1 || !coreinit::NativeHomeSession::SetGamePadRumble(mode != 0)) return -1;
		return 0;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "nn_ccr";
		}

		void RPLMapped() override
		{
			cafeExportRegister("nn_ccr", CCRSysCaffeineBootCheck, LogType::Placeholder);
			if (coreinit::NativeHomeSession::IsChild())
			{
				cafeExportRegister("nn_ccr", CCRSysGetRumbleMode, LogType::InputAPI);
				cafeExportRegister("nn_ccr", CCRSysSetRumbleMode, LogType::InputAPI);
			}
		};

	}s_COSnnccrModule;

	COSModule* GetModule()
	{
		return &s_COSnnccrModule;
	}
}
