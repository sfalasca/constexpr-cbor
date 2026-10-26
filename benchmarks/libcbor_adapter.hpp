#pragma once

// libcbor's own headers declare several C functions under the same names
// tinycbor uses for different purposes (cbor_encode_uint, cbor_encode_tag,
// cbor_encode_null, cbor_encode_double, ...), with different signatures --
// a genuine extern "C" symbol collision, not just an include-path
// ambiguity, so libcbor's own header can never appear in the same
// translation unit as tinycbor's. This thin, libcbor-header-free
// interface is what bench.cpp/correctness.cpp see instead; the real
// #include "libcbor/src/cbor.h" lives only in libcbor_adapter.cpp.
#include <cstdint>
#include <span>

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
