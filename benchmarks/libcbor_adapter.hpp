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

// constexpr_cbor.hpp itself is fine to include here -- the symbol
// collision above is specifically with tinycbor's header, not with this
// project's own code.
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
  long long bytes_allocated;  // building libcbor's own tree from `value` -- libcbor has no
                               // streaming encoder, so this is genuinely part of "encoding"
                               // via its API, not a decode cost being smuggled in
};

// Builds libcbor's own cbor_item_t tree from `value` (the same shared,
// already-decoded ConstexprCbor::CborValue every other library's encode
// benchmark builds its native representation from) and serializes it into
// `out` -- see libcbor_adapter.cpp for why tree-construction time counts
// as encode cost here specifically.
LibcborEncodeResult encode_libcbor(const ConstexprCbor::CborValue& value, std::span<std::uint8_t> out);
