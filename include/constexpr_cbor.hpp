#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// constexpr-cbor: a from-scratch, consteval-evaluable C++20 decoder/encoder for
// RFC 8949 (CBOR). Every decoder here is a pure function over an immutable
// std::span, returning a fresh std::optional -- there is no persistent mutable
// decoder-state object to corrupt on a failed sub-decode.
namespace ConstexprCbor {

// ---------------------------------------------------------------------------
// Negative integers: CBOR encodes major type 1 as -1 - n, n in [0, 2^64-1].
// The true value can be as negative as -2^64, which does not fit std::int64_t
// (min -2^63), so n is kept as-is rather than ever negated into a type that
// cannot hold the result (the exact shape of tinycbor issue #347).
struct NegativeInt {
  std::uint64_t n;  // value == -1 - n
  friend constexpr bool operator==(const NegativeInt&, const NegativeInt&) = default;
  constexpr std::optional<std::int64_t> to_int64() const {
    if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(-1) - static_cast<std::int64_t>(n);
  }
};

// ---------------------------------------------------------------------------
// The shared length-encoding primitive. additional_info 0-23 is a direct
// value; 24/25/26/27 mean "the value is in the following 1/2/4/8 bytes."
// No opinion on major type -- decode_uint, decode_negint, and every
// length-prefixed type (byte/text string, array, map) build on this.
struct DecodedUint {
  std::uint64_t value;
  std::size_t bytes_consumed;
};

constexpr std::optional<DecodedUint> decode_length(std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return std::nullopt;
  }
  const std::uint8_t additional_info = bytes[0] & 0x1f;
  if (additional_info <= 23) {
    return DecodedUint{.value = additional_info, .bytes_consumed = 1};
  }
  if (additional_info > 27) {
    return std::nullopt;  // 28-30 reserved, 31 indefinite: handled by the caller, not here
  }
  constexpr std::size_t length_bytes_for[] = {1, 2, 4, 8};
  const std::size_t length_bytes = length_bytes_for[additional_info - 24];
  if (bytes.size() < 1 + length_bytes) {
    return std::nullopt;
  }
  const auto value_bytes = bytes.subspan(1, length_bytes);
  const std::uint64_t value = std::accumulate(value_bytes.begin(), value_bytes.end(), std::uint64_t{0},
                                               [](std::uint64_t acc, std::uint8_t b) { return (acc << 8) | b; });
  return DecodedUint{.value = value, .bytes_consumed = 1 + length_bytes};
}

// Returns true iff this header's additional info is 31 (the indefinite-length /
// break marker), for whatever major type bytes[0] carries.
constexpr bool is_indefinite_or_break(std::span<const std::uint8_t> bytes) {
  return !bytes.empty() && (bytes[0] & 0x1f) == 31;
}

// ---------------------------------------------------------------------------
// Major type 0: unsigned integer.
constexpr std::optional<DecodedUint> decode_uint(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || (bytes[0] >> 5) != 0) {
    return std::nullopt;
  }
  return decode_length(bytes);
}

// ---------------------------------------------------------------------------
// Major type 1: negative integer.
struct DecodedNegint {
  NegativeInt value;
  std::size_t bytes_consumed;
};

constexpr std::optional<DecodedNegint> decode_negint(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || (bytes[0] >> 5) != 1) {
    return std::nullopt;
  }
  const auto len = decode_length(bytes);
  if (!len) {
    return std::nullopt;
  }
  return DecodedNegint{.value = NegativeInt{.n = len->value}, .bytes_consumed = len->bytes_consumed};
}

// ---------------------------------------------------------------------------
// UTF-8 validation policies for text strings. RFC 8949 Section 5.3.1 says a
// decoder "might or might not want to verify" text-string content is valid
// UTF-8 -- it's an explicit option in the spec, not an oversight, so it's
// modeled as one here: a type supplied as a template parameter, checked
// against an actual interface contract rather than just hoped-for.
template <typename P>
struct is_utf8_validation_policy
    : std::bool_constant<std::is_invocable_r_v<bool, decltype(&P::is_valid), std::span<const std::uint8_t>>> {};

template <typename P>
inline constexpr bool is_utf8_validation_policy_v = is_utf8_validation_policy<P>::value;

// Skip validation entirely -- RFC 8949 permits this, so it's a real, named,
// first-class policy rather than a missing feature.
struct NoUtf8Validation {
  static constexpr bool is_valid(std::span<const std::uint8_t>) { return true; }
};

namespace detail {

// Structural shape only: the right count of 10xxxxxx continuation bytes for
// each lead byte, sequence not truncated. Does NOT reject overlong
// encodings, lone surrogates, or code points past U+10FFFF.
constexpr bool is_structurally_valid_utf8(std::span<const std::uint8_t> bytes) {
  std::size_t i = 0;
  while (i < bytes.size()) {
    const std::uint8_t b = bytes[i];
    std::size_t continuation_count;
    if ((b & 0x80) == 0x00) {
      continuation_count = 0;
    } else if ((b & 0xe0) == 0xc0) {
      continuation_count = 1;
    } else if ((b & 0xf0) == 0xe0) {
      continuation_count = 2;
    } else if ((b & 0xf8) == 0xf0) {
      continuation_count = 3;
    } else {
      return false;  // not a valid lead byte (stray continuation byte, or >4-byte form)
    }
    if (i + continuation_count >= bytes.size() && continuation_count > bytes.size() - i - 1) {
      return false;
    }
    if (bytes.size() - i <= continuation_count) {
      return false;  // truncated: not enough bytes left for the continuations this lead byte demands
    }
    for (std::size_t k = 1; k <= continuation_count; ++k) {
      if ((bytes[i + k] & 0xc0) != 0x80) {
        return false;
      }
    }
    i += continuation_count + 1;
  }
  return true;
}

// Decodes one UTF-8 scalar value starting at bytes[0] (assumed structurally
// valid already), returning the code point and how many bytes it took.
constexpr std::pair<std::uint32_t, std::size_t> decode_one_code_point(std::span<const std::uint8_t> bytes) {
  const std::uint8_t b0 = bytes[0];
  if ((b0 & 0x80) == 0x00) {
    return {b0, 1};
  }
  if ((b0 & 0xe0) == 0xc0) {
    return {(static_cast<std::uint32_t>(b0 & 0x1f) << 6) | (bytes[1] & 0x3f), 2};
  }
  if ((b0 & 0xf0) == 0xe0) {
    return {(static_cast<std::uint32_t>(b0 & 0x0f) << 12) | (static_cast<std::uint32_t>(bytes[1] & 0x3f) << 6) |
                (bytes[2] & 0x3f),
            3};
  }
  return {(static_cast<std::uint32_t>(b0 & 0x07) << 18) | (static_cast<std::uint32_t>(bytes[1] & 0x3f) << 12) |
              (static_cast<std::uint32_t>(bytes[2] & 0x3f) << 6) | (bytes[3] & 0x3f),
          4};
}

}  // namespace detail

struct RoughUtf8Validation {
  static constexpr bool is_valid(std::span<const std::uint8_t> bytes) {
    return detail::is_structurally_valid_utf8(bytes);
  }
};

// Full RFC 3629 conformance: everything RoughUtf8Validation does, plus
// rejects overlong encodings (RFC 8949's own example of well-formed-but-
// invalid CBOR, 0x62c0ae, is exactly this), lone/paired surrogate-half code
// points (U+D800-U+DFFF), and code points beyond U+10FFFF.
struct CompleteUtf8Validation {
  static constexpr bool is_valid(std::span<const std::uint8_t> bytes) {
    if (!detail::is_structurally_valid_utf8(bytes)) {
      return false;
    }
    std::size_t i = 0;
    while (i < bytes.size()) {
      const auto [code_point, length] = detail::decode_one_code_point(bytes.subspan(i));
      // Reject overlong encodings: the shortest form must have been used.
      if (length == 2 && code_point < 0x80) return false;
      if (length == 3 && code_point < 0x800) return false;
      if (length == 4 && code_point < 0x10000) return false;
      // Reject surrogate-half code points and anything past Unicode's range.
      if (code_point >= 0xd800 && code_point <= 0xdfff) return false;
      if (code_point > 0x10ffff) return false;
      i += length;
    }
    return true;
  }
};

// ---------------------------------------------------------------------------
// Major types 2 and 3: byte strings and text strings. Both are a length
// (decoded via decode_length) followed by that many raw bytes; text strings
// add exactly one clause on top, the UTF-8 policy check.
struct DecodedBytes {
  std::span<const std::uint8_t> bytes;
  std::size_t bytes_consumed;
};

constexpr std::optional<DecodedBytes> decode_byte_string(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || (bytes[0] >> 5) != 2) {
    return std::nullopt;
  }
  const auto length_result = decode_length(bytes);
  if (!length_result) {
    return std::nullopt;
  }
  const std::size_t header_size = length_result->bytes_consumed;
  const std::size_t length = length_result->value;
  if (bytes.size() < header_size + length) {
    return std::nullopt;
  }
  return DecodedBytes{.bytes = bytes.subspan(header_size, length), .bytes_consumed = header_size + length};
}

template <typename Utf8Policy = NoUtf8Validation>
constexpr std::optional<DecodedBytes> decode_text_string(std::span<const std::uint8_t> bytes) {
  static_assert(is_utf8_validation_policy_v<Utf8Policy>,
                "Utf8Policy must provide static bool is_valid(std::span<const std::uint8_t>)");
  if (bytes.empty() || (bytes[0] >> 5) != 3) {
    return std::nullopt;
  }
  const auto length_result = decode_length(bytes);
  if (!length_result) {
    return std::nullopt;
  }
  const std::size_t header_size = length_result->bytes_consumed;
  const std::size_t length = length_result->value;
  if (bytes.size() < header_size + length) {
    return std::nullopt;
  }
  const auto payload = bytes.subspan(header_size, length);
  if (!Utf8Policy::is_valid(payload)) {
    return std::nullopt;
  }
  return DecodedBytes{.bytes = payload, .bytes_consumed = header_size + length};
}

// ---------------------------------------------------------------------------
// Major type 7: floats, booleans, null, undefined.
struct DecodedSimple {
  // Simple covers additional info 0-19 (unassigned simple values, still
  // well-formed and decodable per RFC 8949, just with no defined meaning)
  // and the one-byte form (additional info 24, for simple values 32-255).
  enum class Kind { False, True, Null, Undefined, Float, Simple };
  Kind kind;
  double as_double = 0.0;
  std::uint8_t simple_value = 0;
  std::size_t bytes_consumed = 0;
};

constexpr double decode_half_float(std::uint16_t bits) {
  const std::uint64_t sign = static_cast<std::uint64_t>(bits >> 15) & 1u;
  std::uint64_t exponent = (bits >> 10) & 0x1fu;
  std::uint64_t mantissa = bits & 0x3ffu;
  if (exponent == 0) {
    if (mantissa == 0) {
      return std::bit_cast<double>(sign << 63);
    }
    exponent = 1023 - 15 + 1;
    while ((mantissa & 0x400u) == 0) {
      mantissa <<= 1;
      --exponent;
    }
    mantissa &= 0x3ffu;
  } else if (exponent == 0x1fu) {
    exponent = 0x7ffu;
  } else {
    exponent = exponent - 15 + 1023;
  }
  const std::uint64_t bits64 = (sign << 63) | (exponent << 52) | (mantissa << 42);
  return std::bit_cast<double>(bits64);
}

constexpr std::optional<DecodedSimple> decode_simple(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || (bytes[0] >> 5) != 7) {
    return std::nullopt;
  }
  const std::uint8_t additional_info = bytes[0] & 0x1f;
  if (additional_info <= 19) {
    return DecodedSimple{.kind = DecodedSimple::Kind::Simple, .simple_value = additional_info, .bytes_consumed = 1};
  }
  switch (additional_info) {
    case 20:
      return DecodedSimple{.kind = DecodedSimple::Kind::False, .bytes_consumed = 1};
    case 21:
      return DecodedSimple{.kind = DecodedSimple::Kind::True, .bytes_consumed = 1};
    case 22:
      return DecodedSimple{.kind = DecodedSimple::Kind::Null, .bytes_consumed = 1};
    case 23:
      return DecodedSimple{.kind = DecodedSimple::Kind::Undefined, .bytes_consumed = 1};
    case 24: {
      if (bytes.size() < 2) return std::nullopt;
      return DecodedSimple{.kind = DecodedSimple::Kind::Simple, .simple_value = bytes[1], .bytes_consumed = 2};
    }
    case 25: {
      if (bytes.size() < 3) return std::nullopt;
      const auto value_bytes = bytes.subspan(1, 2);
      const std::uint16_t bits = std::accumulate(value_bytes.begin(), value_bytes.end(), std::uint16_t{0},
          [](std::uint16_t a, std::uint8_t b) { return static_cast<std::uint16_t>((a << 8) | b); });
      return DecodedSimple{.kind = DecodedSimple::Kind::Float, .as_double = decode_half_float(bits), .bytes_consumed = 3};
    }
    case 26: {
      if (bytes.size() < 5) return std::nullopt;
      const auto value_bytes = bytes.subspan(1, 4);
      const auto bits = std::accumulate(value_bytes.begin(), value_bytes.end(), std::uint32_t{0},
                                         [](std::uint32_t a, std::uint8_t b) { return (a << 8) | b; });
      return DecodedSimple{.kind = DecodedSimple::Kind::Float,
                            .as_double = static_cast<double>(std::bit_cast<float>(bits)),
                            .bytes_consumed = 5};
    }
    case 27: {
      if (bytes.size() < 9) return std::nullopt;
      const auto value_bytes = bytes.subspan(1, 8);
      const auto bits = std::accumulate(value_bytes.begin(), value_bytes.end(), std::uint64_t{0},
                                         [](std::uint64_t a, std::uint8_t b) { return (a << 8) | b; });
      return DecodedSimple{.kind = DecodedSimple::Kind::Float, .as_double = std::bit_cast<double>(bits), .bytes_consumed = 9};
    }
    default:
      return std::nullopt;  // reserved (28-30), break (31): not well-formed outside an indefinite-length context
  }
}

// ---------------------------------------------------------------------------
// The recursive value type arrays, maps, and tags are built from. Byte/text
// string content here is owned (a copy), not a zero-copy span like
// decode_byte_string/decode_text_string above: an indefinite-length string's
// chunks aren't contiguous in the original buffer (each chunk has its own
// header interspersed), so concatenating them can't stay zero-copy, and
// CborValue needs one uniform representation that covers both the definite
// and indefinite case.
struct CborValue {
  enum class StringKind { Bytes, Text };
  struct OwnedString {
    StringKind kind;
    std::vector<std::uint8_t> content;
    friend constexpr bool operator==(const OwnedString&, const OwnedString&) = default;
  };
  struct Tag {
    std::uint64_t number;
    std::vector<CborValue> value;  // always exactly one element
    friend constexpr bool operator==(const Tag&, const Tag&) = default;
  };
  // Tags 2 and 3 (positive/negative bignum, RFC 8949 Section 3.4.3) are
  // recognized automatically rather than left as an opaque Tag wrapping a
  // byte string: major types 0/1 are wire-format-limited to 64-bit
  // magnitude (no length marker wider than 8 bytes exists), and this is
  // CBOR's actual mechanism for integers past that ceiling, not a detail
  // worth hiding behind a generic tag number a caller has to know to
  // interpret. value == magnitude (tag 2) or -1 - magnitude (tag 3),
  // mirroring NegativeInt's own -1-n convention.
  struct BigNum {
    bool negative;
    std::vector<std::uint8_t> magnitude;  // big-endian
    friend constexpr bool operator==(const BigNum&, const BigNum&) = default;
  };
  // additional info 0-19 (unassigned, still well-formed) or 24 (one-byte
  // form, for 32-255): a simple value with no defined meaning of its own.
  struct SimpleValue {
    std::uint8_t number;
    friend constexpr bool operator==(const SimpleValue&, const SimpleValue&) = default;
  };

  std::variant<std::monostate,              // Undefined
               std::nullptr_t,               // Null
               bool,                         // False/True
               std::uint64_t,                // UnsignedInt
               NegativeInt,                  // NegativeInt
               OwnedString,                  // ByteString or TextString
               BigNum,                       // Tag 2/3: integer magnitude beyond 64 bits
               SimpleValue,                  // Unassigned/extended simple value
               double,                       // Float
               std::vector<CborValue>,       // Array
               std::vector<std::pair<CborValue, CborValue>>,  // Map
               Tag>
      data;

  friend constexpr bool operator==(const CborValue&, const CborValue&) = default;
};

// ---------------------------------------------------------------------------
// Duplicate map keys: RFC 8949 Section 5.3.1 leaves this implementation-
// defined, and there's no settled industry norm (confirmed against QCBOR,
// tinycbor, zcbor, libcbor, NanoCBOR -- none of them treat it as a fixed
// "bug," just an acknowledged open design question). Modeled the same way
// as the UTF-8 policy: a type, checked against a real interface contract.
template <typename P>
struct is_duplicate_key_policy
    : std::bool_constant<std::is_invocable_r_v<
          bool, decltype(&P::on_duplicate), std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue>> {};

template <typename P>
inline constexpr bool is_duplicate_key_policy_v = is_duplicate_key_policy<P>::value;

// on_duplicate is called when `key` has already been seen in `entries`; it
// mutates `entries` as needed and returns false to abort the whole map's
// decode (reject), true to continue.
struct RejectDuplicateKeys {
  static constexpr bool on_duplicate(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue) {
    return false;
  }
};
struct KeepFirstDuplicateKey {
  static constexpr bool on_duplicate(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue) {
    return true;  // first occurrence already in entries; drop this one
  }
};
struct KeepLastDuplicateKey {
  static constexpr bool on_duplicate(std::vector<std::pair<CborValue, CborValue>>& entries, CborValue key,
                                      CborValue value) {
    for (auto& [existing_key, existing_value] : entries) {
      if (existing_key.data == key.data) {
        existing_value = std::move(value);
        break;
      }
    }
    return true;
  }
};

namespace detail {
// Structural equality for the map-key-comparison case above. Only the
// alternatives that can plausibly be map keys need to compare correctly;
// Array/Map/Tag keys are legal CBOR but comparing them structurally is
// handled by std::variant's own operator== via each alternative's own
// operator== (vector, pair, and Tag all get one for free if their members do).
}  // namespace detail

// ---------------------------------------------------------------------------
// The single recursive decoder every major type ultimately goes through.
// MaxDepth bounds how deep arrays/maps/tags may nest -- the single most
// universal hazard surveyed across every real-world CBOR implementation
// checked (QCBOR, tinycbor, and multiple unrelated CVEs all converged on an
// explicit cap). Exceeding it is a refusal, not a crash.
template <std::size_t MaxDepth = 32, typename Utf8Policy = NoUtf8Validation, typename KeyPolicy = RejectDuplicateKeys>
constexpr std::optional<std::pair<CborValue, std::size_t>> decode_value(std::span<const std::uint8_t> bytes,
                                                                         std::size_t depth = 0) {
  if (bytes.empty()) {
    return std::nullopt;
  }
  const std::uint8_t major_type = bytes[0] >> 5;

  switch (major_type) {
    case 0: {
      const auto u = decode_uint(bytes);
      if (!u) return std::nullopt;
      return std::pair{CborValue{.data = u->value}, u->bytes_consumed};
    }
    case 1: {
      const auto n = decode_negint(bytes);
      if (!n) return std::nullopt;
      return std::pair{CborValue{.data = n->value}, n->bytes_consumed};
    }
    case 2:
    case 3: {
      const auto kind = (major_type == 2) ? CborValue::StringKind::Bytes : CborValue::StringKind::Text;
      if (is_indefinite_or_break(bytes)) {
        std::vector<std::uint8_t> content;
        std::size_t consumed = 1;
        while (true) {
          if (consumed >= bytes.size()) return std::nullopt;  // truncated: ran out of buffer before a break
          if (bytes[consumed] == 0xff) {
            ++consumed;
            break;
          }
          const auto chunk = (major_type == 2) ? decode_byte_string(bytes.subspan(consumed))
                                                : decode_text_string<Utf8Policy>(bytes.subspan(consumed));
          if (!chunk) return std::nullopt;  // a chunk must itself be definite-length, same major type
          content.insert(content.end(), chunk->bytes.begin(), chunk->bytes.end());
          consumed += chunk->bytes_consumed;
        }
        return std::pair{CborValue{.data = CborValue::OwnedString{.kind = kind, .content = std::move(content)}},
                          consumed};
      }
      const auto result =
          (major_type == 2) ? decode_byte_string(bytes) : decode_text_string<Utf8Policy>(bytes);
      if (!result) return std::nullopt;
      return std::pair{CborValue{.data = CborValue::OwnedString{
                            .kind = kind, .content = {result->bytes.begin(), result->bytes.end()}}},
                        result->bytes_consumed};
    }
    case 4: {
      if (depth >= MaxDepth) return std::nullopt;
      std::vector<CborValue> items;
      std::size_t consumed = 1;
      if (is_indefinite_or_break(bytes)) {
        while (true) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) { ++consumed; break; }
          const auto item = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(bytes.subspan(consumed), depth + 1);
          if (!item) return std::nullopt;
          items.push_back(std::move(item->first));
          consumed += item->second;
        }
      } else {
        const auto len = decode_length(bytes);
        if (!len) return std::nullopt;
        consumed = len->bytes_consumed;
        for (std::uint64_t i = 0; i < len->value; ++i) {
          if (consumed > bytes.size()) return std::nullopt;
          const auto item = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(bytes.subspan(consumed), depth + 1);
          if (!item) return std::nullopt;
          items.push_back(std::move(item->first));
          consumed += item->second;
        }
      }
      return std::pair{CborValue{.data = std::move(items)}, consumed};
    }
    case 5: {
      if (depth >= MaxDepth) return std::nullopt;
      static_assert(is_duplicate_key_policy_v<KeyPolicy>,
                    "KeyPolicy must provide static bool on_duplicate(vector<pair<CborValue,CborValue>>&, "
                    "CborValue, CborValue)");
      std::vector<std::pair<CborValue, CborValue>> entries;
      std::size_t consumed = 1;
      const bool indefinite = is_indefinite_or_break(bytes);
      std::optional<std::uint64_t> pair_count;
      if (!indefinite) {
        const auto len = decode_length(bytes);
        if (!len) return std::nullopt;
        consumed = len->bytes_consumed;
        pair_count = len->value;
      }
      std::uint64_t pairs_read = 0;
      while (indefinite ? true : pairs_read < *pair_count) {
        if (indefinite) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) { ++consumed; break; }
        }
        const auto key_result = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(bytes.subspan(consumed), depth + 1);
        if (!key_result) return std::nullopt;
        consumed += key_result->second;
        const auto value_result = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(bytes.subspan(consumed), depth + 1);
        if (!value_result) return std::nullopt;
        consumed += value_result->second;

        bool is_duplicate = false;
        for (auto& [existing_key, existing_value] : entries) {
          if (existing_key.data == key_result->first.data) {
            is_duplicate = true;
            break;
          }
        }
        if (is_duplicate) {
          if (!KeyPolicy::on_duplicate(entries, key_result->first, value_result->first)) {
            return std::nullopt;
          }
        } else {
          entries.emplace_back(key_result->first, value_result->first);
        }
        ++pairs_read;
      }
      return std::pair{CborValue{.data = std::move(entries)}, consumed};
    }
    case 6: {
      if (depth >= MaxDepth) return std::nullopt;
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      const auto nested = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(bytes.subspan(len->bytes_consumed), depth + 1);
      if (!nested) return std::nullopt;
      if (len->value == 2 || len->value == 3) {
        if (const auto* bytes_value = std::get_if<CborValue::OwnedString>(&nested->first.data);
            bytes_value && bytes_value->kind == CborValue::StringKind::Bytes) {
          return std::pair{CborValue{.data = CborValue::BigNum{.negative = (len->value == 3),
                                                                 .magnitude = bytes_value->content}},
                            len->bytes_consumed + nested->second};
        }
      }
      std::vector<CborValue> wrapped;
      wrapped.push_back(std::move(nested->first));
      return std::pair{CborValue{.data = CborValue::Tag{.number = len->value, .value = std::move(wrapped)}},
                        len->bytes_consumed + nested->second};
    }
    case 7: {
      const auto s = decode_simple(bytes);
      if (!s) return std::nullopt;
      using K = DecodedSimple::Kind;
      switch (s->kind) {
        case K::False:
          return std::pair{CborValue{.data = false}, s->bytes_consumed};
        case K::True:
          return std::pair{CborValue{.data = true}, s->bytes_consumed};
        case K::Null:
          return std::pair{CborValue{.data = nullptr}, s->bytes_consumed};
        case K::Undefined:
          return std::pair{CborValue{.data = std::monostate{}}, s->bytes_consumed};
        case K::Float:
          return std::pair{CborValue{.data = s->as_double}, s->bytes_consumed};
        case K::Simple:
          return std::pair{CborValue{.data = CborValue::SimpleValue{.number = s->simple_value}}, s->bytes_consumed};
      }
      return std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

// ---------------------------------------------------------------------------
// Encoding. Always emits the shortest ("preferred"/canonical) form for a
// given value, which is what makes byte-for-byte round-trip comparison
// against RFC 8949 Appendix A's own vectors meaningful in the first place.

constexpr void append_header(std::vector<std::uint8_t>& out, std::uint8_t major_type, std::uint64_t value) {
  const std::uint8_t mt = static_cast<std::uint8_t>(major_type << 5);
  if (value <= 23) {
    out.push_back(static_cast<std::uint8_t>(mt | value));
  } else if (value <= 0xff) {
    out.push_back(static_cast<std::uint8_t>(mt | 24));
    out.push_back(static_cast<std::uint8_t>(value));
  } else if (value <= 0xffff) {
    out.push_back(static_cast<std::uint8_t>(mt | 25));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
  } else if (value <= 0xffffffffull) {
    out.push_back(static_cast<std::uint8_t>(mt | 26));
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
  } else {
    out.push_back(static_cast<std::uint8_t>(mt | 27));
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

constexpr std::vector<std::uint8_t> encode_uint(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  append_header(out, 0, value);
  return out;
}

constexpr std::vector<std::uint8_t> encode_negint(NegativeInt value) {
  std::vector<std::uint8_t> out;
  append_header(out, 1, value.n);
  return out;
}

constexpr std::vector<std::uint8_t> encode_byte_string(std::span<const std::uint8_t> content) {
  std::vector<std::uint8_t> out;
  append_header(out, 2, content.size());
  out.insert(out.end(), content.begin(), content.end());
  return out;
}

constexpr std::vector<std::uint8_t> encode_text_string(std::span<const std::uint8_t> content) {
  std::vector<std::uint8_t> out;
  append_header(out, 3, content.size());
  out.insert(out.end(), content.begin(), content.end());
  return out;
}

namespace detail {

constexpr bool bit_identical_double(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::isnan(a) && std::isnan(b);
  }
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Derives a candidate half-float bit pattern, then verifies it round-trips
// through decode_half_float back to the exact original value before
// trusting it -- a hand-derived inverse is exactly the kind of code that's
// easy to get subtly wrong, so the actual decoder is used as the oracle
// rather than trusting the forward derivation blindly.
constexpr std::optional<std::uint16_t> try_encode_half_float(double value) {
  std::uint16_t candidate;
  if (std::isnan(value)) {
    candidate = 0x7e00;  // canonical NaN
  } else {
    const std::uint64_t bits64 = std::bit_cast<std::uint64_t>(value);
    // Kept as uint64_t (not narrowed to uint16_t) so every `sign << 15`
    // below stays in an unsigned type at least as wide as int, the same
    // discipline used throughout this header: a narrower unsigned type
    // promotes to a signed int before shifting, which is where the
    // genuinely subtle (if ultimately harmless here) sign-conversion
    // warnings further down came from.
    const std::uint64_t sign = bits64 >> 63;
    if (value == 0.0) {
      candidate = static_cast<std::uint16_t>(sign << 15);
    } else if (std::isinf(value)) {
      candidate = static_cast<std::uint16_t>((sign << 15) | (0x1fu << 10));
    } else {
      const std::int64_t exponent = static_cast<std::int64_t>((bits64 >> 52) & 0x7ff) - 1023;
      const std::uint64_t mantissa = bits64 & 0xfffffffffffffull;
      if (exponent > 15) {
        return std::nullopt;
      }
      if (exponent >= -14) {
        const std::uint64_t half_exp = static_cast<std::uint64_t>(exponent + 15);
        const std::uint64_t half_mantissa = mantissa >> 42;
        candidate = static_cast<std::uint16_t>((sign << 15) | (half_exp << 10) | half_mantissa);
      } else {
        const std::int64_t shift = -14 - exponent;
        if (shift > 10) {
          return std::nullopt;
        }
        const std::uint64_t full_mantissa = mantissa | (1ull << 52);
        const auto needed_shift = static_cast<unsigned>(42 + shift);
        const std::uint64_t half_mantissa = full_mantissa >> needed_shift;
        candidate = static_cast<std::uint16_t>((sign << 15) | half_mantissa);
      }
    }
  }
  if (!bit_identical_double(decode_half_float(candidate), value)) {
    return std::nullopt;
  }
  return candidate;
}

constexpr std::optional<std::uint32_t> try_encode_single_float(double value) {
  if (std::isnan(value)) {
    return std::uint32_t{0x7fc00000};  // canonical single NaN
  }
  const float as_float = static_cast<float>(value);
  if (!bit_identical_double(static_cast<double>(as_float), value)) {
    return std::nullopt;
  }
  return std::bit_cast<std::uint32_t>(as_float);
}

}  // namespace detail

constexpr std::vector<std::uint8_t> encode_float(double value) {
  if (const auto h = detail::try_encode_half_float(value)) {
    return {0xf9, static_cast<std::uint8_t>(*h >> 8), static_cast<std::uint8_t>(*h)};
  }
  if (const auto s = detail::try_encode_single_float(value)) {
    std::vector<std::uint8_t> out{0xfa};
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(*s >> shift));
    return out;
  }
  const std::uint64_t d = std::isnan(value) ? 0x7ff8000000000000ull : std::bit_cast<std::uint64_t>(value);
  std::vector<std::uint8_t> out{0xfb};
  for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(d >> shift));
  return out;
}

template <std::size_t MaxDepth = 32, typename Utf8Policy = NoUtf8Validation, typename KeyPolicy = RejectDuplicateKeys>
constexpr std::vector<std::uint8_t> encode_value(const CborValue& value) {
  return std::visit(
      [](const auto& v) -> std::vector<std::uint8_t> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return {0xf7};  // undefined
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
          return {0xf6};  // null
        } else if constexpr (std::is_same_v<T, bool>) {
          return {v ? std::uint8_t{0xf5} : std::uint8_t{0xf4}};
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          return encode_uint(v);
        } else if constexpr (std::is_same_v<T, NegativeInt>) {
          return encode_negint(v);
        } else if constexpr (std::is_same_v<T, CborValue::OwnedString>) {
          return (v.kind == CborValue::StringKind::Bytes) ? encode_byte_string(v.content)
                                                            : encode_text_string(v.content);
        } else if constexpr (std::is_same_v<T, CborValue::SimpleValue>) {
          if (v.number <= 23) {
            return std::vector<std::uint8_t>{static_cast<std::uint8_t>(0xe0 | v.number)};
          }
          return std::vector<std::uint8_t>{0xf8, v.number};
        } else if constexpr (std::is_same_v<T, CborValue::BigNum>) {
          std::vector<std::uint8_t> out;
          append_header(out, 6, v.negative ? std::uint64_t{3} : std::uint64_t{2});
          const auto bignum_bytes = encode_byte_string(v.magnitude);
          out.insert(out.end(), bignum_bytes.begin(), bignum_bytes.end());
          return out;
        } else if constexpr (std::is_same_v<T, double>) {
          return encode_float(v);
        } else if constexpr (std::is_same_v<T, std::vector<CborValue>>) {
          std::vector<std::uint8_t> out;
          append_header(out, 4, v.size());
          for (const auto& item : v) {
            const auto item_bytes = encode_value<MaxDepth, Utf8Policy, KeyPolicy>(item);
            out.insert(out.end(), item_bytes.begin(), item_bytes.end());
          }
          return out;
        } else if constexpr (std::is_same_v<T, std::vector<std::pair<CborValue, CborValue>>>) {
          std::vector<std::uint8_t> out;
          append_header(out, 5, v.size());
          for (const auto& [key, val] : v) {
            const auto key_bytes = encode_value<MaxDepth, Utf8Policy, KeyPolicy>(key);
            out.insert(out.end(), key_bytes.begin(), key_bytes.end());
            const auto val_bytes = encode_value<MaxDepth, Utf8Policy, KeyPolicy>(val);
            out.insert(out.end(), val_bytes.begin(), val_bytes.end());
          }
          return out;
        } else if constexpr (std::is_same_v<T, CborValue::Tag>) {
          std::vector<std::uint8_t> out;
          append_header(out, 6, v.number);
          const auto nested_bytes = encode_value<MaxDepth, Utf8Policy, KeyPolicy>(v.value.front());
          out.insert(out.end(), nested_bytes.begin(), nested_bytes.end());
          return out;
        }
      },
      value.data);
}

}  // namespace ConstexprCbor
