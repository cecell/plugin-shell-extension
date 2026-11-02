// zlib (RFC 1950/1951) decompression for compressed plugin records.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pse {

// Inflates a zlib stream into exactly `expectedSize` bytes. Returns false on
// a malformed stream or a size mismatch.
bool ZlibInflate(const uint8_t* src, size_t srcLen, size_t expectedSize, std::vector<uint8_t>* out);

}  // namespace pse
