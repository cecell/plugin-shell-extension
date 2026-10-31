#pragma once

#include <windows.h>
#include <unknwn.h>

namespace pp {

// Live COM objects + server locks; DllCanUnloadNow returns S_OK at zero.
extern volatile long g_dllRefs;

HRESULT CreateClassFactory(REFIID riid, void** ppv);

}  // namespace pp
