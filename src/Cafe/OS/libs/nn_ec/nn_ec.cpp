#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/nn_ec/nn_ec.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/CafeSystem.h"

typedef struct  
{
	/* +0x00 */ uint8 ukn00[0x10];
	/* +0x10 */ uint8 ukn10[0x0C];
	/* +0x2C */ uint8 ukn2C[0x10]; // currency string?
	/* +0x3C */ uint32 ukn3C; // money amount?
	// size of struct is 0x40
}nnEcUknMoneyStruct_t;

void nnEcExport___ct__Q3_2nn2ec5MoneyFPCcN21(PPCInterpreter_t* hCPU)
{
	debug_printf("nn_ec.__ct__Q3_2nn2ec5MoneyFPCcN21(0x%08x, ...)\n", hCPU->gpr[3]);
	nnEcUknMoneyStruct_t* moneyStruct = (nnEcUknMoneyStruct_t*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	// todo -> Setup struct
	osLib_returnFromFunction(hCPU, memory_getVirtualOffsetFromPointer(moneyStruct));
}

namespace nn::ec
{
	uint32 GetErrorCode(betype<nnResult>* result)
	{
		// EC delegates ACT results to the account error converter. A zero code
		// turns Dimensions' offline-account notice into fatal error 199-9998.
		if (CafeSystem::GetForegroundTitleId() == 0x0005000010194200ull &&
			CafeSystem::GetForegroundTitleVersion() == 320 &&
			CafeSystem::GetRPXHashUpdated() == 0x84227143 && result)
		{
			const nnResult value = *result;
			if ((value & 0x80000000) && (value & 0x18000000) != 0x18000000 &&
				nnResult_GetModule(value) == NN_RESULT_MODULE_NN_ACT)
				return static_cast<uint32>(NNResultToErrorCode(value, NN_RESULT_MODULE_NN_ACT));
		}
		// Preserve the previous unimplemented return for unverified paths.
		return 0;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "nn_ec";
		}

		void RPLMapped() override
		{
			cafeExportRegisterFunc(GetErrorCode, "nn_ec", "GetErrorCode__Q2_2nn2ecFRCQ2_2nn6Result", LogType::Placeholder);
			osLib_addFunction("nn_ec", "__ct__Q3_2nn2ec5MoneyFPCcN21", nnEcExport___ct__Q3_2nn2ec5MoneyFPCcN21);
		};

	}s_COSnnEcModule;

	COSModule* GetModule()
	{
		return &s_COSnnEcModule;
	}
}
