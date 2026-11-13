#pragma once

// libcbor's header declares C functions (cbor_encode_uint, cbor_encode_tag, ...)
// under the same names as tinycbor's but with different signatures, so the two
// headers cannot share a translation unit. This libcbor-header-free interface is
// what bench.cpp and correctness.cpp see; the real include lives in
// libcbor_adapter.cpp.
#include <cstdint>
#include <span>

#include "constexpr_cbor.hpp"

struct LibcborDecodeResult {
  bool ok;
  std::uint64_t items;
  std::int64_t isum;
  double fsum;
  std::uint64_t strbytes;
  long long bytes_allocated;  // measured while the decoded tree is still alive
};

LibcborDecodeResult decode_libcbor(std::span<const std::uint8_t> bytes);

bool accepted_by_libcbor(std::span<const std::uint8_t> bytes);

struct LibcborEncodeResult {
  bool ok;
  std::size_t bytes_written;
  long long bytes_allocated;  // building libcbor's tree from `value` (it has no streaming encoder)
};

// Builds libcbor's cbor_item_t tree from `value` and serializes it into `out`.
// Tree construction counts as encode cost, since libcbor has no streaming encoder.
LibcborEncodeResult encode_libcbor(const ConstexprCbor::CborValue& value, std::span<std::uint8_t> out);
