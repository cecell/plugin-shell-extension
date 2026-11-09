// Shows a .bsa as a read-only folder in Explorer, the way Windows shows a zip.
// Files and folders can be browsed, copied and dragged out. Nothing can be
// added, renamed or deleted.
#pragma once

#include <windows.h>

namespace pse {

HRESULT CreateBsaFolderFactory(REFIID riid, void** ppv);

}  // namespace pse
