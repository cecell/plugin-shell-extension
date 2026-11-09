// Explorer property handler for Skyrim asset files that have none of their own:
// .tga dimensions and bit depth, .fuz duration, bit rate, sample rate and
// channels. It fills Windows' own properties, so the existing columns, details
// pane and tooltips show them, using beth-asset-metadata to read the headers.
//
// .dds, .xwm and .wav aren't here because other handlers own them (Windows, the
// XWM property handler), and an extension can only have one.
#pragma once

#include <windows.h>

namespace pse {

HRESULT CreateAssetPropertyHandlerFactory(REFIID riid, void** ppv);

}  // namespace pse
