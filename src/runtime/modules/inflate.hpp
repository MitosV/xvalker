#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xv::runtime::modules {
// Inflates a zlib stream (RFC 1950) onto the end of `out`; false if it is not one.
// `limit` is the most bytes the stream may hold: 0 for the default (64 MiB).
bool inflate_zlib(std::span<const std::uint8_t> stream, std::vector<std::uint8_t> &out,
                  std::size_t limit = 0);
} // namespace xv::runtime::modules
