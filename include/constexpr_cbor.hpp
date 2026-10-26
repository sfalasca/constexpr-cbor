#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

namespace detail {
// Narrows an already-bounds-checked value from `From` to `To`, without
// ever writing a cast that's a no-op on some target and a real narrowing
// on another. size_t and uint64_t are the identical type on a 64-bit
// target (where a static_cast between them is a literal no-op GCC's
// -Wuseless-cast flags), but genuinely different widths on a 32-bit one
// (wasm32 via Emscripten, or a 32-bit embedded target) -- there, the
// static_cast is the correct, intentional way to perform a value that's
// already been proven in-range, not an accident -Wshorten-64-to-32 (part
// of Clang's -Wconversion) should flag. if constexpr picks whichever of
// those two is actually true for the target at hand, so only one of the
// two ever exists in the compiled output.
template <typename To, typename From>
constexpr To narrow(From v) {
  if constexpr (std::is_same_v<To, From>) {
    return v;
  } else {
    return static_cast<To>(v);
  }
}
}  // namespace detail

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

// Stricter than valid UTF-8, not another strictness level of it: ASCII is
// a subset of UTF-8 (any byte 0x00-0x7F is already a complete, single-byte
// UTF-8 code point), but the reverse isn't true -- "café" is
// perfectly valid UTF-8 and still fails this policy. For systems or
// protocol fields that can't handle multi-byte sequences at all, not for
// deciding how strictly to check UTF-8 itself.
struct AsciiOnlyValidation {
  static constexpr bool is_valid(std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t b : bytes) {
      if (b > 0x7f) {
        return false;
      }
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
  const std::size_t length = detail::narrow<std::size_t>(length_result->value);
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
  const std::size_t length = detail::narrow<std::size_t>(length_result->value);
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

// Forward declaration only: CborValue's own templated constructor/.get<T>()
// fall back to this (ADL user-type hook dispatch, see the definition below
// this class) for any T that isn't one of the built-in CBOR alternatives.
// Declaring it here (not yet defining it) is enough, the same forward-
// declare-then-define-later pattern already used for Arena::NodePool below.
template <typename T, typename = void>
struct CborSerializer;

struct CborValue;

// ---------------------------------------------------------------------------
// The recursive value type arrays, maps, and tags are built from. Byte/text
// string content here is owned (a copy), not a zero-copy span like
// decode_byte_string/decode_text_string above: an indefinite-length string's
// chunks aren't contiguous in the original buffer (each chunk has its own
// header interspersed), so concatenating them can't stay zero-copy, and
// CborValue needs one uniform representation that covers both the definite
// and indefinite case.
//
// Beyond the raw variant, CborValue also provides an nlohmann::json-style
// ergonomic interface (is_*()/operator[]/.get<T>()/iteration/brace-init
// construction) -- entirely additive, kept exception-free throughout (no
// throwing anywhere: a miss/type-mismatch returns a shared sentinel or
// std::optional, never throws, preserving -fno-exceptions compatibility and
// consteval-safety for every existing decode/encode path). Adding the
// builder constructor below necessarily makes CborValue a non-aggregate
// (a deliberate, accepted break from the old CborValue{.data = ...}
// construction syntax -- .data itself stays public and unchanged).
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

  constexpr CborValue() = default;

  // The three self-referential/recursive alternatives (containing CborValue
  // itself, directly or via pair) get their OWN constructor, member-
  // initializing `data` directly via the variant's own constructor, rather
  // than default-constructing `data` to monostate first and then switching
  // it to a different active alternative via assignment in the body below
  // -- a strictly better, more direct construction path under GCC/libstdc++
  // (this project's only toolchain with full, verified support; see
  // README's "Toolchain support" section). Note for the record: this does
  // NOT make these three alternatives constructible under Clang -- Clang's
  // handling of std::variant's in_place_index-based constructor for a
  // self-referential alternative this deep fails ("placement new would
  // change type of storage from '__aligned_membuf<...>' to '...'") even for
  // a single, fresh, first-ever construction, with no prior "switch"
  // involved at all. Confirmed empirically (including against the
  // pre-ergonomics commit, before this constructor existed at all) to be a
  // pre-existing Clang/libstdc++ limitation this codebase cannot work
  // around from user code, not something introduced or fixable here.
  template <typename T>
    requires(std::same_as<std::remove_cvref_t<T>, std::vector<CborValue>> ||
             std::same_as<std::remove_cvref_t<T>, std::vector<std::pair<CborValue, CborValue>>> ||
             std::same_as<std::remove_cvref_t<T>, Tag>)
  constexpr CborValue(T&& v) : data(std::forward<T>(v)) {}  // NOLINT(google-explicit-constructor)

  // The single converting constructor every other ergonomic construction
  // path goes through -- one if-constexpr dispatch chain (matching the
  // codebase's own encode_value-visitor idiom) rather than a proliferation
  // of overloads, which would recreate exactly the overload-ambiguity
  // problems described below for the array/map literal case. Excludes
  // T == CborValue itself so it never hijacks the implicitly-declared
  // copy/move constructor (the classic "forwarding reference shadows copy
  // constructor" trap), and excludes the three recursive alternatives
  // above (handled by the dedicated constructor instead).
  template <typename T>
    requires(!std::same_as<std::remove_cvref_t<T>, CborValue> &&
             !std::same_as<std::remove_cvref_t<T>, std::vector<CborValue>> &&
             !std::same_as<std::remove_cvref_t<T>, std::vector<std::pair<CborValue, CborValue>>> &&
             !std::same_as<std::remove_cvref_t<T>, Tag>)
  constexpr CborValue(T&& v) {  // NOLINT(google-explicit-constructor)
    using D = std::remove_cvref_t<T>;
    if constexpr (std::same_as<D, std::monostate> || std::same_as<D, std::nullptr_t> || std::same_as<D, bool> ||
                  std::same_as<D, std::uint64_t> || std::same_as<D, NegativeInt> || std::same_as<D, OwnedString> ||
                  std::same_as<D, BigNum> || std::same_as<D, SimpleValue> || std::same_as<D, double>) {
      data = std::forward<T>(v);
    } else if constexpr (std::integral<D> && !std::same_as<D, bool>) {
      const std::int64_t iv = static_cast<std::int64_t>(v);
      if (iv >= 0) {
        data = static_cast<std::uint64_t>(iv);
      } else {
        data = NegativeInt{.n = static_cast<std::uint64_t>(-1 - iv)};
      }
    } else if constexpr (std::same_as<D, float>) {
      data = static_cast<double>(v);
    } else if constexpr (std::constructible_from<std::string_view, T>) {
      const std::string_view sv(v);
      data = OwnedString{.kind = StringKind::Text, .content = {sv.begin(), sv.end()}};
    } else {
      if (!CborSerializer<D>::to_cbor(*this, v)) {
        data = std::monostate{};
      }
    }
  }

  template <typename T>
    requires(!std::same_as<std::remove_cvref_t<T>, CborValue>)
  constexpr CborValue& operator=(T&& v) {
    *this = CborValue(std::forward<T>(v));
    return *this;
  }

  // Array-vs-map brace-init disambiguation, adapted from nlohmann's own
  // mechanism (a single constructor + a runtime heuristic over the
  // materialized elements, not plain overloading -- nlohmann itself
  // abandoned overloading for the exact same ambiguity a nested braced
  // element like `{"a", 1}` creates against either an initializer_list<T>
  // or an initializer_list<pair<T,T>> element type). Generalized here vs.
  // nlohmann's string-only-key heuristic: CBOR map keys are unrestricted,
  // so the rule is purely structural -- every element a 2-element array, not
  // "every element a 2-element array whose first is a string."
  static constexpr CborValue build_array_or_map(std::initializer_list<CborValue> init, bool type_deduction) {
    bool all_pairs = type_deduction && init.size() > 0;
    if (all_pairs) {
      for (const auto& elem : init) {
        const auto* arr = std::get_if<std::vector<CborValue>>(&elem.data);
        if (!arr || arr->size() != 2) {
          all_pairs = false;
          break;
        }
      }
    }
    if (all_pairs) {
      std::vector<std::pair<CborValue, CborValue>> entries;
      for (const auto& elem : init) {
        const auto& arr = std::get<std::vector<CborValue>>(elem.data);
        entries.emplace_back(arr[0], arr[1]);
      }
      return CborValue(std::move(entries));
    }
    return CborValue(std::vector<CborValue>(init.begin(), init.end()));
  }

  constexpr CborValue(std::initializer_list<CborValue> init, bool type_deduction = true)
      // Delegates to the move constructor (itself constructing `data`
      // directly via the recursive constructor above) rather than default-
      // constructing `data` and assigning into it here.
      : CborValue(build_array_or_map(init, type_deduction)) {}  // NOLINT(google-explicit-constructor)

  // Forces array interpretation, bypassing the heuristic above -- covers
  // `{}` (ambiguous between empty array/empty map) and any genuine array
  // of 2-element sub-arrays the heuristic would otherwise misread as a map.
  static constexpr CborValue array(std::initializer_list<CborValue> init = {}) { return CborValue(init, false); }

  // Forces map interpretation; the always-unambiguous spelling, especially
  // once keys aren't plain strings: object({ {1u, "one"}, {array({1,2}),
  // "composite-key"} }). Every element must be a 2-element array (key,
  // value) -- not re-checked here, same discipline as the rest of this
  // ergonomic layer trusting caller-authored-in-code input.
  static constexpr CborValue object(std::initializer_list<CborValue> init) {
    std::vector<std::pair<CborValue, CborValue>> entries;
    for (const auto& elem : init) {
      const auto& arr = std::get<std::vector<CborValue>>(elem.data);
      entries.emplace_back(arr[0], arr[1]);
    }
    return CborValue(std::move(entries));
  }

  // A dedicated factory, not an implicit path: a bare byte sequence is
  // exactly as ambiguous with "array of small integers" as in nlohmann,
  // which solved it identically with json::binary(...).
  static constexpr CborValue binary(std::span<const std::uint8_t> bytes) {
    return CborValue(OwnedString{.kind = StringKind::Bytes, .content = {bytes.begin(), bytes.end()}});
  }

  constexpr bool is_undefined() const { return std::holds_alternative<std::monostate>(data); }
  constexpr bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
  constexpr bool is_boolean() const { return std::holds_alternative<bool>(data); }
  constexpr bool is_unsigned() const { return std::holds_alternative<std::uint64_t>(data); }
  constexpr bool is_negative_int() const { return std::holds_alternative<NegativeInt>(data); }
  constexpr bool is_bignum() const { return std::holds_alternative<BigNum>(data); }
  constexpr bool is_integer() const { return is_unsigned() || is_negative_int() || is_bignum(); }
  constexpr bool is_float() const { return std::holds_alternative<double>(data); }
  constexpr bool is_number() const { return is_integer() || is_float(); }
  constexpr bool is_string() const {
    const auto* s = std::get_if<OwnedString>(&data);
    return s != nullptr && s->kind == StringKind::Text;
  }
  constexpr bool is_binary() const {
    const auto* s = std::get_if<OwnedString>(&data);
    return s != nullptr && s->kind == StringKind::Bytes;
  }
  constexpr bool is_simple() const { return std::holds_alternative<SimpleValue>(data); }
  constexpr bool is_array() const { return std::holds_alternative<std::vector<CborValue>>(data); }
  constexpr bool is_map() const { return std::holds_alternative<std::vector<std::pair<CborValue, CborValue>>>(data); }
  constexpr bool is_object() const { return is_map(); }
  constexpr bool is_tag() const { return std::holds_alternative<Tag>(data); }

  // Matches nlohmann's own convention: 0 for null/undefined, 1 for any
  // other scalar, element/pair count for array/map.
  constexpr std::size_t size() const {
    if (is_undefined() || is_null()) return 0;
    if (const auto* arr = std::get_if<std::vector<CborValue>>(&data)) return arr->size();
    if (const auto* m = std::get_if<std::vector<std::pair<CborValue, CborValue>>>(&data)) return m->size();
    return 1;
  }
  constexpr bool empty() const { return size() == 0; }

  constexpr bool contains(const CborValue& key) const {
    const auto* entries = std::get_if<std::vector<std::pair<CborValue, CborValue>>>(&data);
    if (!entries) return false;
    for (const auto& [k, v] : *entries) {
      if (k.data == key.data) return true;
    }
    return false;
  }
  // No separate std::string_view overload: a string literal already
  // converts to CborValue implicitly via the converting constructor above,
  // and adding one creates a genuine ambiguity (both std::string_view and
  // CborValue are reachable from `const char*` via exactly one user-defined
  // conversion, so overload resolution can't prefer either).

  // Declared here, defined out-of-line after null_sentinel (below this
  // class) is complete -- the miss-case return path needs a CborValue to
  // bind a reference to, and a static member of CborValue's own type can't
  // be initialized inside this still-incomplete class body.
  constexpr const CborValue& operator[](std::size_t i) const;
  constexpr CborValue& operator[](std::size_t i);
  constexpr const CborValue& operator[](const CborValue& key) const;
  constexpr CborValue& operator[](const CborValue& key);

  template <typename T>
  constexpr std::optional<T> get() const {
    if constexpr (std::same_as<T, bool>) {
      const auto* p = std::get_if<bool>(&data);
      return p ? std::optional<T>(*p) : std::nullopt;
    } else if constexpr (std::same_as<T, std::uint64_t>) {
      const auto* p = std::get_if<std::uint64_t>(&data);
      return p ? std::optional<T>(*p) : std::nullopt;
    } else if constexpr (std::same_as<T, std::int64_t>) {
      if (const auto* p = std::get_if<std::uint64_t>(&data)) {
        if (*p <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return static_cast<std::int64_t>(*p);
        }
        return std::nullopt;
      }
      if (const auto* p = std::get_if<NegativeInt>(&data)) {
        return p->to_int64();
      }
      return std::nullopt;
    } else if constexpr (std::same_as<T, double>) {
      const auto* p = std::get_if<double>(&data);
      return p ? std::optional<T>(*p) : std::nullopt;
    } else if constexpr (std::same_as<T, std::string>) {
      const auto* p = std::get_if<OwnedString>(&data);
      if (p && p->kind == StringKind::Text) return std::string(p->content.begin(), p->content.end());
      return std::nullopt;
    } else if constexpr (std::same_as<T, std::vector<std::uint8_t>>) {
      const auto* p = std::get_if<OwnedString>(&data);
      if (p && p->kind == StringKind::Bytes) return p->content;
      return std::nullopt;
    } else if constexpr (std::same_as<T, std::monostate>) {
      return is_undefined() ? std::optional<T>(std::monostate{}) : std::nullopt;
    } else if constexpr (std::same_as<T, std::nullptr_t>) {
      return is_null() ? std::optional<T>(nullptr) : std::nullopt;
    } else {
      T value{};
      if (CborSerializer<T>::from_cbor(*this, value)) return value;
      return std::nullopt;
    }
  }

  // Iteration: array yields elements in order; map yields values only
  // (matching nlohmann's own `for (auto& v : j)` default). No primitive
  // iteration arm -- iterating a scalar is simply not supported here.
  struct const_iterator {
    std::variant<std::vector<CborValue>::const_iterator, std::vector<std::pair<CborValue, CborValue>>::const_iterator>
        it;
    constexpr const CborValue& operator*() const {
      return std::visit(
          [](const auto& i) -> const CborValue& {
            using It = std::decay_t<decltype(i)>;
            if constexpr (std::is_same_v<It, std::vector<CborValue>::const_iterator>) {
              return *i;
            } else {
              return i->second;
            }
          },
          it);
    }
    constexpr const_iterator& operator++() {
      std::visit([](auto& i) { ++i; }, it);
      return *this;
    }
    constexpr bool operator==(const const_iterator& other) const { return it == other.it; }
  };

  constexpr const_iterator begin() const {
    if (const auto* arr = std::get_if<std::vector<CborValue>>(&data)) return const_iterator{arr->begin()};
    if (const auto* m = std::get_if<std::vector<std::pair<CborValue, CborValue>>>(&data))
      return const_iterator{m->begin()};
    return const_iterator{std::vector<CborValue>::const_iterator{}};
  }
  constexpr const_iterator end() const {
    if (const auto* arr = std::get_if<std::vector<CborValue>>(&data)) return const_iterator{arr->end()};
    if (const auto* m = std::get_if<std::vector<std::pair<CborValue, CborValue>>>(&data))
      return const_iterator{m->end()};
    return const_iterator{std::vector<CborValue>::const_iterator{}};
  }

  // .items(): key/value pairs for a map; an empty range for anything else
  // (not ill-formed -- this is ergonomic sugar, not decode validation).
  struct ItemsProxy {
    std::vector<std::pair<CborValue, CborValue>>* entries;
    struct Item {
      const CborValue& key;
      CborValue& value;
    };
    struct iterator {
      std::vector<std::pair<CborValue, CborValue>>::iterator it;
      constexpr Item operator*() const { return Item{it->first, it->second}; }
      constexpr iterator& operator++() {
        ++it;
        return *this;
      }
      constexpr bool operator==(const iterator& other) const { return it == other.it; }
    };
    constexpr iterator begin() const { return entries ? iterator{entries->begin()} : iterator{}; }
    constexpr iterator end() const { return entries ? iterator{entries->end()} : iterator{}; }
  };
  constexpr ItemsProxy items() {
    if (auto* m = std::get_if<std::vector<std::pair<CborValue, CborValue>>>(&data)) return ItemsProxy{m};
    return ItemsProxy{nullptr};
  }
};

// Shared, namespace-scope (not function-local -- see the comment on
// detail::call_to_cbor below for why that distinction matters here too)
// sentinel every miss-case const operator[] below binds its return
// reference to, giving `j["x"]["y"]` chainability even through a miss
// (detect it via .is_undefined()). inline constexpr gives exactly one
// address across translation units and is constant-initialized, so it's
// simultaneously usable inside consteval evaluation, at ordinary runtime,
// and safe under concurrent read access (it is never mutated).
inline constexpr CborValue null_sentinel{};

constexpr const CborValue& CborValue::operator[](std::size_t i) const {
  if (const auto* arr = std::get_if<std::vector<CborValue>>(&data); arr && i < arr->size()) {
    return (*arr)[i];
  }
  return null_sentinel;
}

constexpr CborValue& CborValue::operator[](std::size_t i) {
  if (!std::holds_alternative<std::vector<CborValue>>(data)) {
    data = std::vector<CborValue>{};
  }
  auto& arr = std::get<std::vector<CborValue>>(data);
  if (i >= arr.size()) {
    arr.resize(i + 1);
  }
  return arr[i];
}

constexpr const CborValue& CborValue::operator[](const CborValue& key) const {
  if (const auto* entries = std::get_if<std::vector<std::pair<CborValue, CborValue>>>(&data)) {
    for (const auto& [k, v] : *entries) {
      if (k.data == key.data) return v;
    }
  }
  return null_sentinel;
}

constexpr CborValue& CborValue::operator[](const CborValue& key) {
  if (!std::holds_alternative<std::vector<std::pair<CborValue, CborValue>>>(data)) {
    data = std::vector<std::pair<CborValue, CborValue>>{};
  }
  auto& entries = std::get<std::vector<std::pair<CborValue, CborValue>>>(data);
  for (auto& [k, v] : entries) {
    if (k.data == key.data) return v;
  }
  entries.emplace_back(key, CborValue{});
  return entries.back().second;
}

// ---------------------------------------------------------------------------
// to_cbor/from_cbor ADL hooks: mirrors nlohmann's adl_serializer pattern.
// The real reason for this indirection isn't "ADL needs a wrapper" (an
// unqualified call already gets ADL) -- it's that it creates a second,
// independent customization point for types a user can't attach a free
// function to at all (third-party/std types), via specializing
// CborSerializer<T> directly instead.
//
// call_to_cbor/call_from_cbor MUST be free functions, not members of
// CborSerializer itself: inside a member function body, unqualified lookup
// finds the enclosing class's own member of the same name first (if
// CborSerializer<T>::to_cbor called an unqualified `to_cbor(...)` directly,
// it would find itself and recurse, never reaching ADL -- ordinary lookup
// finding a class member suppresses ADL entirely, per [basic.lookup.argdep]).
// A free function has no enclosing class, so ordinary lookup here finds
// nothing and ADL fires normally against T's own associated namespaces.
namespace detail {
template <typename T>
concept HasFreeToCbor = requires(CborValue& out, const T& v) {
  { to_cbor(out, v) } -> std::convertible_to<bool>;
};
template <typename T>
concept HasFreeFromCbor = requires(const CborValue& in, T& v) {
  { from_cbor(in, v) } -> std::convertible_to<bool>;
};

template <typename T>
constexpr bool call_to_cbor(CborValue& out, const T& value) {
  return to_cbor(out, value);
}
template <typename T>
constexpr bool call_from_cbor(const CborValue& in, T& value) {
  return from_cbor(in, value);
}
}  // namespace detail

template <typename T, typename>
struct CborSerializer {
  static constexpr bool to_cbor(CborValue& out, const T& value) {
    static_assert(detail::HasFreeToCbor<T>,
                  "No to_cbor(CborValue&, const T&) found via ADL and no CborSerializer<T> specialization provided");
    if constexpr (detail::HasFreeToCbor<T>) {
      return detail::call_to_cbor(out, value);
    } else {
      return false;
    }
  }
  static constexpr bool from_cbor(const CborValue& in, T& value) {
    static_assert(
        detail::HasFreeFromCbor<T>,
        "No from_cbor(const CborValue&, T&) found via ADL and no CborSerializer<T> specialization provided");
    if constexpr (detail::HasFreeFromCbor<T>) {
      return detail::call_from_cbor(in, value);
    } else {
      return false;
    }
  }
};

// Forward declarations only: ConstexprCbor::Arena::NodePool/Node/IndexCell
// are defined later in this header (after this ConstexprCbor namespace
// closes), but the duplicate-key policies just below need to declare an
// Arena-shaped on_duplicate overload alongside their std::vector-shaped
// one, so that the exact same policy TYPE works for both decode_value
// (this namespace) and Arena::decode_value -- "it's still a policy" means
// one coherent type per policy, not a parallel Arena-only copy of each.
// Declaring (not yet defining) these here is enough: the member function
// templates below are only instantiated once actually called, by which
// point Arena's own definitions have long since appeared in the same
// translation unit.
namespace Arena {
template <std::size_t MaxNodes, std::size_t MaxBytes>
struct NodePool;
struct Node;
struct IndexCell;
}  // namespace Arena

// ---------------------------------------------------------------------------
// Duplicate map keys: RFC 8949 Section 5.3.1 leaves this implementation-
// defined, and there's no settled industry norm (confirmed against QCBOR,
// tinycbor, zcbor, libcbor, NanoCBOR -- none of them treat it as a fixed
// "bug," just an acknowledged open design question). Modeled the same way
// as the UTF-8 policy: a type, checked against a real interface contract.
// A concrete target function-pointer type, not a bare `decltype(&P::on_duplicate)`,
// is required here: once the Arena-shaped on_duplicate overload below
// exists too (a member function *template*), an unqualified address-of
// has no target type to resolve the overload against and is ill-formed
// ("decltype cannot resolve address of overloaded function") even for a
// concrete, non-template P. The static_cast pins exactly this
// std::vector-shaped overload, leaving the Arena-shaped template one to
// its own trait in constexpr_cbor_arena.hpp.
// Partial specialization + void_t, not a plain std::is_invocable_r_v on a
// bare decltype(&P::on_duplicate): the static_cast that pins the overload
// is itself ill-formed for a P with the wrong shape entirely (casting
// between two unrelated function-pointer types isn't just "not
// invocable," it's a hard error) -- and a class template's base-class
// list isn't a SFINAE context, so that error would abort compilation
// instead of making the trait evaluate to false. The void_t in the
// partial specialization's template argument list is a SFINAE context:
// substitution failure there is quietly rejected, selecting the primary
// (false) template instead, which is exactly what lets this trait still
// report false (rather than hard-erroring) for a mismatched P, so the
// friendly static_assert message above actually fires.
template <typename P, typename = void>
struct is_duplicate_key_policy : std::false_type {};

template <typename P>
struct is_duplicate_key_policy<
    P, std::void_t<decltype(static_cast<bool (*)(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue)>(
           &P::on_duplicate))>>
    : std::true_type {};

template <typename P>
inline constexpr bool is_duplicate_key_policy_v = is_duplicate_key_policy<P>::value;

// on_duplicate is called when `key` has already been seen in `entries`; it
// mutates `entries` as needed and returns false to abort the whole map's
// decode (reject), true to continue. Each policy below also provides an
// Arena-shaped overload (same decision, same return-value contract, just
// expressed over NodePool indices instead of a std::vector<pair<...>> --
// Arena::decode_value has no flat, directly-comparable entries container
// to search or mutate, only a pool and indices into it): `existing_pair_index`
// is which already-written pair (0-based) the new key matched;
// `new_key_index`/`new_value_index` are where the newly-decoded pair's key
// and value already live in the pool.
struct RejectDuplicateKeys {
  static constexpr bool on_duplicate(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue) {
    return false;
  }
  template <std::size_t MaxNodes, std::size_t MaxBytes>
  static constexpr bool on_duplicate(Arena::NodePool<MaxNodes, MaxBytes>&, std::size_t /*first_pair*/,
                                      std::size_t /*existing_pair_index*/, std::size_t /*new_key_index*/,
                                      std::size_t /*new_value_index*/) {
    return false;
  }
};
struct KeepFirstDuplicateKey {
  static constexpr bool on_duplicate(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue) {
    return true;  // first occurrence already in entries; drop this one
  }
  template <std::size_t MaxNodes, std::size_t MaxBytes>
  static constexpr bool on_duplicate(Arena::NodePool<MaxNodes, MaxBytes>&, std::size_t /*first_pair*/,
                                      std::size_t /*existing_pair_index*/, std::size_t /*new_key_index*/,
                                      std::size_t /*new_value_index*/) {
    return true;  // first occurrence's pair is left exactly where it is; drop this one
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
  // Defined out-of-line, after Arena::Node/IndexCell are complete: GCC's
  // -Wtemplate-body checks aggregate-initializer syntax against a member
  // template's body eagerly, even though it's only truly instantiated
  // once called (by which point Arena's own types have long since been
  // fully defined later in this header) -- an inline body here would hit
  // that check while IndexCell is still just a forward declaration.
  template <std::size_t MaxNodes, std::size_t MaxBytes>
  static constexpr bool on_duplicate(Arena::NodePool<MaxNodes, MaxBytes>& pool, std::size_t first_pair,
                                      std::size_t existing_pair_index, std::size_t new_key_index,
                                      std::size_t new_value_index);
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
      return std::pair{CborValue(u->value), u->bytes_consumed};
    }
    case 1: {
      const auto n = decode_negint(bytes);
      if (!n) return std::nullopt;
      return std::pair{CborValue(n->value), n->bytes_consumed};
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
        return std::pair{CborValue(CborValue::OwnedString{.kind = kind, .content = std::move(content)}), consumed};
      }
      const auto result =
          (major_type == 2) ? decode_byte_string(bytes) : decode_text_string<Utf8Policy>(bytes);
      if (!result) return std::nullopt;
      return std::pair{
          CborValue(CborValue::OwnedString{.kind = kind, .content = {result->bytes.begin(), result->bytes.end()}}),
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
      return std::pair{CborValue(std::move(items)), consumed};
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
      return std::pair{CborValue(std::move(entries)), consumed};
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
          return std::pair{
              CborValue(CborValue::BigNum{.negative = (len->value == 3), .magnitude = bytes_value->content}),
              len->bytes_consumed + nested->second};
        }
      }
      std::vector<CborValue> wrapped;
      wrapped.push_back(std::move(nested->first));
      return std::pair{CborValue(CborValue::Tag{.number = len->value, .value = std::move(wrapped)}),
                        len->bytes_consumed + nested->second};
    }
    case 7: {
      const auto s = decode_simple(bytes);
      if (!s) return std::nullopt;
      using K = DecodedSimple::Kind;
      switch (s->kind) {
        case K::False:
          return std::pair{CborValue(false), s->bytes_consumed};
        case K::True:
          return std::pair{CborValue(true), s->bytes_consumed};
        case K::Null:
          return std::pair{CborValue(nullptr), s->bytes_consumed};
        case K::Undefined:
          return std::pair{CborValue(std::monostate{}), s->bytes_consumed};
        case K::Float:
          return std::pair{CborValue(s->as_double), s->bytes_consumed};
        case K::Simple:
          return std::pair{CborValue(CborValue::SimpleValue{.number = s->simple_value}), s->bytes_consumed};
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
