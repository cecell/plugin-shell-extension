#pragma once

#include <windows.h>

namespace pse {

HRESULT CreateFilePropertyHandlerFactory(REFIID riid, void** ppv);

}  // namespace pse
