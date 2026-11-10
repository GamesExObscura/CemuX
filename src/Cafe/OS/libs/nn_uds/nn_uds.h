#pragma once
#include "Cafe/OS/RPL/COSModule.h"
namespace nn::uds {
COSModule* GetModule();
// Retire the private UDS alarm before coreinit tears down its alarm registry.
void OnTitleStop();
}
