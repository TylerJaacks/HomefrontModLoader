// Internal to the loader DLL.
#pragma once

#include "hfsdk_native.h"

namespace loader {

void Log(const char* fmt, ...);
const HFManagedApi& Managed();          // zeroed until the SDK has started
void FillNativeApi(HFNativeApi& api);  // native_api.cpp

// vac.cpp
bool DisableVac();                      // before the server runs; false: unknown exe bytes, don't run
void CheckVac(unsigned long now_ms);    // every tick: stops the server if Steam reports it secure

}  // namespace loader
