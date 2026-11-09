// Explorer property handler for Skyrim asset files that have none of their own:
// .tga dimensions and bit depth, .fuz duration, bit rate, sample rate and
// channels. It fills Windows' own properties, so the existing columns, details
// pane and tooltips show them, using beth-asset-metadata to read the headers.
//
// .xwm and .wav aren't here because other handlers own them (the XWM property
// handler, Windows), and an extension can only have one.
//
// .dds is owned by Windows too, but its handler is worth keeping, so the .dds
// handler here creates it, passes all of its values through and adds Mipmaps.
#pragma once

#include <windows.h>

namespace pse {

HRESULT CreateAssetPropertyHandlerFactory(REFIID riid, void** ppv);
HRESULT CreateDdsPropertyHandlerFactory(REFIID riid, void** ppv);

}  // namespace pse
