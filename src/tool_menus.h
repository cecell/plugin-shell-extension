// Right-click menus: "BSA manager" on .bsa files, and "Hashes" on every file.
#pragma once

#include <windows.h>

namespace pse {

HRESULT CreateBsaManagerFactory(REFIID riid, void** ppv);
HRESULT CreateBsaDropFactory(REFIID riid, void** ppv);
HRESULT CreateHashMenuFactory(REFIID riid, void** ppv);
HRESULT CreatePapyrusMenuFactory(REFIID riid, void** ppv);

}  // namespace pse
