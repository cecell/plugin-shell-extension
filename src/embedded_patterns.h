// The subrecord pattern catalog compiled into the DLL (data/itm_patterns.json).
#pragma once

#include "pattern_engine.h"

namespace pse {

// Parsed once, on first use; null if the resource is missing or malformed.
const PatternCatalog* EmbeddedPatterns();

}  // namespace pse
