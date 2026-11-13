#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
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
#include <version>

#if defined(__has_include)
#if __has_include(<expected>)
#include <expected>
#endif
#endif

// constexpr-cbor: header-only, consteval-evaluable C++20 codec for RFC 8949.
// Decoders are pure functions over an immutable std::span and return a fresh
// Expected, so there is no mutable decoder state to corrupt on a failed decode.
namespace ConstexprCbor {

// ---------------------------------------------------------------------------
// Every failure carries an ErrorReason and, where meaningful, a byte offset
// (absolute within the input span when decoding, the output span when encoding).
// TypeMismatch and UnsupportedUserType are not buffer-position failures; their
// offset is 0.
// wire format broke down at some point."
enum class ErrorReason {
  TruncatedInput,        // ran out of bytes before a complete header/field
  MalformedHeader,       // reserved additional-info (28-30), wrong major type for
                          // context, or a break marker where one isn't permitted
  TruncatedPayload,      // a declared length/argument exceeds what's actually left
  MaxDepthExceeded,
  InvalidUtf8,
  DuplicateKey,          // rejected by the active KeyPolicy
  IntegerOutOfRange,     // NegativeInt::to_int64 / get<int64_t> narrowing failure
  TypeMismatch,          // get<T>() / NoAllocView::get<T>() wrong active alternative
  UnsupportedUserType,   // CborSerializer<T>::from_cbor returned false
  NodeCapacityExhausted,  // NoAlloc only
  RefCapacityExhausted,   // NoAlloc only
  ByteCapacityExhausted,  // NoAlloc only (decode-side storage, or NoAllocBuilder's
                          // make_text/make_bytes)
  CountOverflow,          // NoAlloc only -- checked_node_count's uint64_t->size_t / *2 guard
  KeyTrackingCapacityExhausted,  // Sax only -- SaxRange's MaxTrackedKeys exhausted
  OutputBufferTooSmall,   // encode_value (NoAlloc only -- heap's std::vector can't fail this way)
};

struct Error {
  ErrorReason reason;
  std::size_t offset = 0;
  friend constexpr bool operator==(const Error&, const Error&) = default;
};

namespace detail {

// Expected<T, E> is std::expected when the compiler provides it (detected via
// __cpp_lib_expected), otherwise a small fallback. libstdc++ gates std::expected
// on C++23, so using it unconditionally would raise the library's C++20 minimum.
#if defined(__cpp_lib_expected)

template <typename T, typename E>
using ExpectedImpl = std::expected<T, E>;
using std::unexpected;

#else

template <typename E>
struct Unexpected {
  E error;
};
template <typename E>
constexpr Unexpected<E> unexpected(E e) {
  return Unexpected<E>{e};
}

// Only the operations this library and its tests use; no and_then/transform/or_else.
template <typename T, typename E>
class ExpectedImpl {
  std::variant<T, E> storage_;

 public:
  // Forwarding reference rather than by-value-then-move: moving a materialized
  // std::string into the variant's union storage breaks constexpr evaluation on
  // arm-none-eabi-g++ 13.2.1 (newlib libstdc++).
  // Excludes Unexpected<E> and ExpectedImpl itself so copy/move are not shadowed.
  template <typename U = T>
    requires(!std::same_as<std::remove_cvref_t<U>, Unexpected<E>> &&
             !std::same_as<std::remove_cvref_t<U>, ExpectedImpl> && std::constructible_from<T, U&&>)
  constexpr ExpectedImpl(U&& v) : storage_(std::in_place_index<0>, std::forward<U>(v)) {}  // NOLINT(google-explicit-constructor)
  constexpr ExpectedImpl(Unexpected<E> u)                                                  // NOLINT(google-explicit-constructor)
      : storage_(std::in_place_index<1>, std::move(u.error)) {}

  constexpr bool has_value() const { return storage_.index() == 0; }
  constexpr explicit operator bool() const { return has_value(); }
  constexpr const T& operator*() const { return std::get<0>(storage_); }
  constexpr const T* operator->() const { return &std::get<0>(storage_); }
  constexpr const E& error() const { return std::get<1>(storage_); }
  // Compare directly against a value convertible to T, like std::optional.
  // Hidden friend, so C++20 synthesizes the reversed and != forms.
  template <typename U>
  friend constexpr bool operator==(const ExpectedImpl& lhs, const U& rhs) {
    if (!lhs.has_value()) return false;
    // cmp_equal avoids -Wsign-compare on mixed-signedness integral comparisons.
    if constexpr (std::integral<T> && std::integral<U>) {
      return std::cmp_equal(std::get<0>(lhs.storage_), rhs);
    } else {
      return std::get<0>(lhs.storage_) == rhs;
    }
  }
  template <typename U>
  constexpr T value_or(U&& fallback) const {
    return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }
  // Throws like std::expected::value(); aborts under -fno-exceptions.
  constexpr const T& value() const {
    if (!has_value()) {
#if defined(__cpp_exceptions)
      throw std::bad_variant_access();
#else
      std::abort();
#endif
    }
    return std::get<0>(storage_);
  }
};

#endif  // defined(__cpp_lib_expected)

}  // namespace detail

template <typename T, typename E>
using Expected = detail::ExpectedImpl<T, E>;
using detail::unexpected;

// ---------------------------------------------------------------------------
// Negative integers: major type 1 encodes -1 - n for n in [0, 2^64-1].
// The value can reach -2^64, below int64_t's range, so n is stored unnegated
// (cf. tinycbor issue #347).
struct NegativeInt {
  std::uint64_t n;  // value == -1 - n
  friend constexpr bool operator==(const NegativeInt&, const NegativeInt&) = default;
  constexpr Expected<std::int64_t, Error> to_int64() const {
    if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return unexpected(Error{ErrorReason::IntegerOutOfRange});
    }
    return static_cast<std::int64_t>(-1) - static_cast<std::int64_t>(n);
  }
};

namespace detail {
// Narrows a value already proven in range. size_t and uint64_t are the same type
// on 64-bit targets (where a static_cast trips -Wuseless-cast) but differ on
// 32-bit ones (wasm32, embedded), so the cast is chosen with if constexpr.
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
// Shared length decoding. Additional info 0-23 is the value itself; 24/25/26/27
// mean it follows in the next 1/2/4/8 bytes. The major type is not checked;
// every length-prefixed type builds on this.
struct DecodedUint {
  std::uint64_t value;
  std::size_t bytes_consumed;
};

constexpr Expected<DecodedUint, Error> decode_length(std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return unexpected(Error{ErrorReason::TruncatedInput});
  }
  const std::uint8_t additional_info = bytes[0] & 0x1f;
  if (additional_info <= 23) {
    return DecodedUint{.value = additional_info, .bytes_consumed = 1};
  }
  if (additional_info > 27) {
    return unexpected(Error{ErrorReason::MalformedHeader});  // 28-30 reserved, 31 indefinite: handled by the caller
  }
  constexpr std::size_t length_bytes_for[] = {1, 2, 4, 8};
  const std::size_t length_bytes = length_bytes_for[additional_info - 24];
  if (bytes.size() < 1 + length_bytes) {
    return unexpected(Error{ErrorReason::TruncatedInput});
  }
  const auto value_bytes = bytes.subspan(1, length_bytes);
  const std::uint64_t value = std::accumulate(value_bytes.begin(), value_bytes.end(), std::uint64_t{0},
                                               [](std::uint64_t acc, std::uint8_t b) { return (acc << 8) | b; });
  return DecodedUint{.value = value, .bytes_consumed = 1 + length_bytes};
}

// True iff the header's additional info is 31 (indefinite length or break).
constexpr bool is_indefinite_or_break(std::span<const std::uint8_t> bytes) {
  return !bytes.empty() && (bytes[0] & 0x1f) == 31;
}

// ---------------------------------------------------------------------------
// Major type 0: unsigned integer.
constexpr Expected<DecodedUint, Error> decode_uint(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || (bytes[0] >> 5) != 0) {
    return unexpected(Error{ErrorReason::MalformedHeader});
  }
  return decode_length(bytes);
}

// ---------------------------------------------------------------------------
// Major type 1: negative integer.
struct DecodedNegint {
  NegativeInt value;
  std::size_t bytes_consumed;
};

constexpr Expected<DecodedNegint, Error> decode_negint(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || (bytes[0] >> 5) != 1) {
    return unexpected(Error{ErrorReason::MalformedHeader});
  }
  const auto len = decode_length(bytes);
  if (!len) {
    return unexpected(len.error());
  }
  return DecodedNegint{.value = NegativeInt{.n = len->value}, .bytes_consumed = len->bytes_consumed};
}

// ---------------------------------------------------------------------------
// UTF-8 validation policies for text strings. RFC 8949 Section 5.3.1 makes
// validation optional, so it is a template parameter checked against an
// interface contract.
template <typename P>
struct is_utf8_validation_policy
    : std::bool_constant<std::is_invocable_r_v<bool, decltype(&P::is_valid), std::span<const std::uint8_t>>> {};

template <typename P>
inline constexpr bool is_utf8_validation_policy_v = is_utf8_validation_policy<P>::value;

// Skip validation (permitted by RFC 8949).
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

// Full RFC 3629 conformance: also rejects overlong encodings (RFC 8949's 0x62c0ae
// example), surrogate halves (U+D800-U+DFFF), and code points beyond U+10FFFF.
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

// ASCII only. Not a strictness level of UTF-8: valid UTF-8 such as "café" fails
// this policy.
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
// Major types 2 and 3: a length followed by that many bytes.
// Text strings additionally run the UTF-8 policy.
struct DecodedBytes {
  std::span<const std::uint8_t> bytes;
  std::size_t bytes_consumed;
};

constexpr Expected<DecodedBytes, Error> decode_byte_string(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || (bytes[0] >> 5) != 2) {
    return unexpected(Error{ErrorReason::MalformedHeader});
  }
  const auto length_result = decode_length(bytes);
  if (!length_result) {
    return unexpected(length_result.error());
  }
  const std::size_t header_size = length_result->bytes_consumed;
  const std::size_t length = detail::narrow<std::size_t>(length_result->value);
  if (bytes.size() < header_size + length) {
    return unexpected(Error{ErrorReason::TruncatedPayload});
  }
  return DecodedBytes{.bytes = bytes.subspan(header_size, length), .bytes_consumed = header_size + length};
}

template <typename Utf8Policy = NoUtf8Validation>
constexpr Expected<DecodedBytes, Error> decode_text_string(std::span<const std::uint8_t> bytes) {
  static_assert(is_utf8_validation_policy_v<Utf8Policy>,
                "Utf8Policy must provide static bool is_valid(std::span<const std::uint8_t>)");
  if (bytes.empty() || (bytes[0] >> 5) != 3) {
    return unexpected(Error{ErrorReason::MalformedHeader});
  }
  const auto length_result = decode_length(bytes);
  if (!length_result) {
    return unexpected(length_result.error());
  }
  const std::size_t header_size = length_result->bytes_consumed;
  const std::size_t length = detail::narrow<std::size_t>(length_result->value);
  if (bytes.size() < header_size + length) {
    return unexpected(Error{ErrorReason::TruncatedPayload});
  }
  const auto payload = bytes.subspan(header_size, length);
  if (!Utf8Policy::is_valid(payload)) {
    return unexpected(Error{ErrorReason::InvalidUtf8});
  }
  return DecodedBytes{.bytes = payload, .bytes_consumed = header_size + length};
}

// ---------------------------------------------------------------------------
// Major type 7: floats, booleans, null, undefined.
struct DecodedSimple {
  // Simple: unassigned values (additional info 0-19, or 24 for 32-255).
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

constexpr Expected<DecodedSimple, Error> decode_simple(std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || (bytes[0] >> 5) != 7) {
    return unexpected(Error{ErrorReason::MalformedHeader});
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
      if (bytes.size() < 2) return unexpected(Error{ErrorReason::TruncatedInput});
      return DecodedSimple{.kind = DecodedSimple::Kind::Simple, .simple_value = bytes[1], .bytes_consumed = 2};
    }
    case 25: {
      if (bytes.size() < 3) return unexpected(Error{ErrorReason::TruncatedInput});
      const auto value_bytes = bytes.subspan(1, 2);
      const std::uint16_t bits = std::accumulate(value_bytes.begin(), value_bytes.end(), std::uint16_t{0},
          [](std::uint16_t a, std::uint8_t b) { return static_cast<std::uint16_t>((a << 8) | b); });
      return DecodedSimple{.kind = DecodedSimple::Kind::Float, .as_double = decode_half_float(bits), .bytes_consumed = 3};
    }
    case 26: {
      if (bytes.size() < 5) return unexpected(Error{ErrorReason::TruncatedInput});
      const auto value_bytes = bytes.subspan(1, 4);
      const auto bits = std::accumulate(value_bytes.begin(), value_bytes.end(), std::uint32_t{0},
                                         [](std::uint32_t a, std::uint8_t b) { return (a << 8) | b; });
      return DecodedSimple{.kind = DecodedSimple::Kind::Float,
                            .as_double = static_cast<double>(std::bit_cast<float>(bits)),
                            .bytes_consumed = 5};
    }
    case 27: {
      if (bytes.size() < 9) return unexpected(Error{ErrorReason::TruncatedInput});
      const auto value_bytes = bytes.subspan(1, 8);
      const auto bits = std::accumulate(value_bytes.begin(), value_bytes.end(), std::uint64_t{0},
                                         [](std::uint64_t a, std::uint8_t b) { return (a << 8) | b; });
      return DecodedSimple{.kind = DecodedSimple::Kind::Float, .as_double = std::bit_cast<double>(bits), .bytes_consumed = 9};
    }
    default:
      // reserved (28-30), break (31): not well-formed outside an indefinite-length context
      return unexpected(Error{ErrorReason::MalformedHeader});
  }
}

// Shifts a nested error's offset (relative to its own sub-span) by `base`, making
// it absolute within the caller's buffer.
constexpr Error rebase_error(Error e, std::size_t base) {
  e.offset += base;
  return e;
}

// Forward declaration: CborValue falls back to CborSerializer<T> (ADL hook
// dispatch, defined below) for types that are not built-in alternatives.
template <typename T, typename = void>
struct CborSerializer;

// Customization point: specialize in namespace ConstexprCbor to describe T's
// fields as (name, member pointer) pairs, e.g.:
//
//   namespace ConstexprCbor {
//   template <> constexpr auto cbor_fields<mylib::Point>() {
//     return std::tuple{std::pair{"x", &mylib::Point::x},
//                        std::pair{"y", &mylib::Point::y}};
//   }
//   }
//
// CborSerializer<T> builds to_cbor/from_cbor from it when T has no ADL hook.
template <typename T>
auto cbor_fields();

struct CborValue;

// ---------------------------------------------------------------------------
// The recursive value type. String content is owned (copied) because an
// indefinite-length string's chunks are not contiguous in the input.
//
// Also offers an nlohmann::json-style interface (is_*(), operator[], get<T>(),
// iteration, brace-init). It never throws: a miss or type mismatch yields a
// sentinel or Expected, keeping it -fno-exceptions and consteval safe.
// The brace-init constructor makes CborValue a non-aggregate, so
// CborValue{.data = ...} no longer compiles; .data itself remains public.
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
  // Tags 2 and 3 (bignums, RFC 8949 Section 3.4.3) are recognized rather than
  // left as an opaque Tag, since they are how CBOR represents integers beyond
  // 64 bits. value == magnitude (tag 2) or -1 - magnitude (tag 3).
  struct BigNum {
    bool negative;
    std::vector<std::uint8_t> magnitude;  // big-endian
    friend constexpr bool operator==(const BigNum&, const BigNum&) = default;
  };
  // Unassigned simple value (additional info 0-19, or 24 for 32-255).
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

  // The recursive alternatives (those containing CborValue) are initialized
  // directly through the variant constructor rather than default-constructing
  // monostate and then assigning.
  // Clang with libstdc++ still rejects these alternatives ("placement new would
  // change type of storage"), even on first construction. This is a pre-existing
  // toolchain limitation (see the README's toolchain support section).
  template <typename T>
    requires(std::same_as<std::remove_cvref_t<T>, std::vector<CborValue>> ||
             std::same_as<std::remove_cvref_t<T>, std::vector<std::pair<CborValue, CborValue>>> ||
             std::same_as<std::remove_cvref_t<T>, Tag>)
  constexpr CborValue(T&& v) : data(std::forward<T>(v)) {}  // NOLINT(google-explicit-constructor)

  // Single converting constructor for all other alternatives, dispatched with
  // if constexpr because overloads would be ambiguous for array/map literals.
  // Excludes CborValue (so copy/move are not hijacked) and the recursive
  // alternatives handled above.
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

  // Array-vs-map brace-init disambiguation, as in nlohmann: a runtime check over
  // the built elements, because overloading is ambiguous for nested braces like
  // {"a", 1}. CBOR keys are unrestricted, so the rule is purely structural:
  // every element a 2-element array means map.
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
      // Delegate so `data` is constructed directly.
      : CborValue(build_array_or_map(init, type_deduction)) {}  // NOLINT(google-explicit-constructor)

  // Forces array interpretation: covers `{}` and arrays of 2-element sub-arrays
  // that the heuristic would read as a map.
  static constexpr CborValue array(std::initializer_list<CborValue> init = {}) { return CborValue(init, false); }

  // Forces map interpretation, e.g. object({{1u, "one"}, {array({1,2}), "k"}}).
  // Each element must be a 2-element array (key, value); this is not re-checked.
  static constexpr CborValue object(std::initializer_list<CborValue> init) {
    std::vector<std::pair<CborValue, CborValue>> entries;
    for (const auto& elem : init) {
      const auto& arr = std::get<std::vector<CborValue>>(elem.data);
      entries.emplace_back(arr[0], arr[1]);
    }
    return CborValue(std::move(entries));
  }

  // Factory rather than implicit conversion: a byte sequence is ambiguous with an
  // array of small integers (nlohmann uses json::binary the same way).
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
  // No string_view overload: string literals already convert to CborValue, and a
  // second conversion path would make calls ambiguous.

  // Defined after null_sentinel: the miss path needs a CborValue to reference,
  // and a static member of the incomplete class cannot hold one.
  constexpr const CborValue& operator[](std::size_t i) const;
  constexpr CborValue& operator[](std::size_t i);
  constexpr const CborValue& operator[](const CborValue& key) const;
  constexpr CborValue& operator[](const CborValue& key);

  template <typename T>
  constexpr Expected<T, Error> get() const {
    if constexpr (std::same_as<T, bool>) {
      const auto* p = std::get_if<bool>(&data);
      return p ? Expected<T, Error>(*p) : unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::uint64_t>) {
      const auto* p = std::get_if<std::uint64_t>(&data);
      return p ? Expected<T, Error>(*p) : unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::int64_t>) {
      if (const auto* p = std::get_if<std::uint64_t>(&data)) {
        if (*p <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return static_cast<std::int64_t>(*p);
        }
        return unexpected(Error{ErrorReason::IntegerOutOfRange});
      }
      if (const auto* p = std::get_if<NegativeInt>(&data)) {
        return p->to_int64();
      }
      return unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, double>) {
      const auto* p = std::get_if<double>(&data);
      return p ? Expected<T, Error>(*p) : unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::string>) {
      const auto* p = std::get_if<OwnedString>(&data);
      if (p && p->kind == StringKind::Text) return std::string(p->content.begin(), p->content.end());
      return unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::vector<std::uint8_t>>) {
      const auto* p = std::get_if<OwnedString>(&data);
      if (p && p->kind == StringKind::Bytes) return p->content;
      return unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::monostate>) {
      return is_undefined() ? Expected<T, Error>(std::monostate{}) : unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::nullptr_t>) {
      return is_null() ? Expected<T, Error>(nullptr) : unexpected(Error{ErrorReason::TypeMismatch});
    } else {
      T value{};
      if (CborSerializer<T>::from_cbor(*this, value)) return value;
      return unexpected(Error{ErrorReason::UnsupportedUserType});
    }
  }

  // Iteration: arrays yield elements, maps yield values only (as in nlohmann).
  // Scalars are not iterable.
  struct const_iterator {
    using value_type = CborValue;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

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
    constexpr const_iterator operator++(int) {
      const_iterator tmp = *this;
      ++*this;
      return tmp;
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

  // .items(): key/value pairs for a map; an empty range for anything else.
  struct ItemsProxy {
    std::vector<std::pair<CborValue, CborValue>>* entries;
    struct Item {
      const CborValue& key;
      CborValue& value;
    };
    struct iterator {
      using value_type = Item;
      using difference_type = std::ptrdiff_t;
      using iterator_category = std::forward_iterator_tag;

      std::vector<std::pair<CborValue, CborValue>>::iterator it;
      constexpr Item operator*() const { return Item{it->first, it->second}; }
      constexpr iterator& operator++() {
        ++it;
        return *this;
      }
      constexpr iterator operator++(int) {
        iterator tmp = *this;
        ++*this;
        return tmp;
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

// Returned by const operator[] on a miss, so `j["x"]["y"]` chains through misses
// (test with .is_undefined()). inline constexpr gives one address across
// translation units and works in consteval and at runtime.
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
// to_cbor/from_cbor ADL hooks, as in nlohmann's adl_serializer.
// Specializing CborSerializer<T> directly covers types that cannot take a free
// function (third-party and std types).
//
// call_to_cbor/call_from_cbor must be free functions: inside a CborSerializer
// member, an unqualified `to_cbor(...)` finds the member itself, which
// suppresses ADL and recurses.
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

// cbor_fields<T>() fallback, lower priority than the ADL hooks. True exactly when
// the user specialized it (the primary template is undefined, so other types
// fail substitution instead of linking).
template <typename T>
concept HasCborFields = requires { cbor_fields<T>(); };

// Pulls the member's value type out of a `Member T::*` pointer type, so
// cbor_fields_from_cbor knows what to .get<>() for each field.
template <typename M>
struct member_pointer_value_type;
template <typename C, typename M>
struct member_pointer_value_type<M C::*> {
  using type = M;
};
template <typename M>
using member_pointer_value_type_t = typename member_pointer_value_type<std::remove_cvref_t<M>>::type;

template <typename T>
constexpr bool cbor_fields_to_cbor(CborValue& out, const T& value) {
  std::vector<std::pair<CborValue, CborValue>> entries;
  std::apply(
      [&](auto const&... field) {
        (entries.emplace_back(CborValue(std::string_view(field.first)), CborValue(value.*(field.second))), ...);
      },
      cbor_fields<T>());
  out = CborValue(std::move(entries));
  return true;
}

template <typename T>
constexpr bool cbor_fields_from_cbor(const CborValue& in, T& value) {
  // get<T>() only recognizes canonical scalar types, so integral and floating
  // members are read as int64_t/double and narrowed per field.
  bool ok = true;
  std::apply(
      [&](auto const&... field) {
        ([&] {
           using Member = member_pointer_value_type_t<decltype(field.second)>;
           const CborValue& field_value = in[std::string_view(field.first)];
           if constexpr (std::same_as<Member, bool>) {
             auto decoded = field_value.get<bool>();
             if (!decoded) {
               ok = false;
               return;
             }
             value.*(field.second) = *decoded;
           } else if constexpr (std::integral<Member>) {
             auto decoded = field_value.get<std::int64_t>();
             if (!decoded) {
               ok = false;
               return;
             }
             value.*(field.second) = static_cast<Member>(*decoded);
           } else if constexpr (std::floating_point<Member>) {
             auto decoded = field_value.get<double>();
             if (!decoded) {
               ok = false;
               return;
             }
             value.*(field.second) = static_cast<Member>(*decoded);
           } else {
             auto decoded = field_value.template get<Member>();
             if (!decoded) {
               ok = false;
               return;
             }
             value.*(field.second) = *decoded;
           }
         }(),
         ...);
      },
      cbor_fields<T>());
  return ok;
}
}  // namespace detail

template <typename T, typename>
struct CborSerializer {
  static constexpr bool to_cbor(CborValue& out, const T& value) {
    if constexpr (detail::HasFreeToCbor<T>) {
      return detail::call_to_cbor(out, value);
    } else if constexpr (detail::HasCborFields<T>) {
      return detail::cbor_fields_to_cbor(out, value);
    } else {
      static_assert(detail::HasFreeToCbor<T> || detail::HasCborFields<T>,
                    "No to_cbor(CborValue&, const T&) found via ADL, no cbor_fields<T>() specialization, and no "
                    "CborSerializer<T> specialization provided");
      return false;
    }
  }
  static constexpr bool from_cbor(const CborValue& in, T& value) {
    if constexpr (detail::HasFreeFromCbor<T>) {
      return detail::call_from_cbor(in, value);
    } else if constexpr (detail::HasCborFields<T>) {
      return detail::cbor_fields_from_cbor(in, value);
    } else {
      static_assert(detail::HasFreeFromCbor<T> || detail::HasCborFields<T>,
                    "No from_cbor(const CborValue&, T&) found via ADL, no cbor_fields<T>() specialization, and no "
                    "CborSerializer<T> specialization provided");
      return false;
    }
  }
};

// Forward declarations so the duplicate-key policies below can declare
// NoAlloc-shaped on_duplicate overloads, letting one policy type serve both
// decoders. NodePool is a non-owning view over caller-supplied storage.
namespace NoAlloc {
struct NodePool;
struct Node;
}  // namespace NoAlloc

// ---------------------------------------------------------------------------
// Duplicate map keys: RFC 8949 Section 5.3.1 leaves handling
// implementation-defined and the surveyed libraries (QCBOR, tinycbor, zcbor,
// libcbor, NanoCBOR) disagree, so it is a policy type like the UTF-8 one.
//
// The trait pins the std::vector overload with a static_cast, because
// &P::on_duplicate is ambiguous once the NoAlloc overload exists. The cast is a
// hard error for a mis-shaped P, so it sits in a void_t partial specialization
// (a SFINAE context) to make the trait false instead of aborting compilation
// before the static_assert can fire.
template <typename P, typename = void>
struct is_duplicate_key_policy : std::false_type {};

template <typename P>
struct is_duplicate_key_policy<
    P, std::void_t<decltype(static_cast<bool (*)(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue)>(
           &P::on_duplicate))>>
    : std::true_type {};

template <typename P>
inline constexpr bool is_duplicate_key_policy_v = is_duplicate_key_policy<P>::value;

// on_duplicate runs when `key` is already in `entries`. It may mutate `entries`
// and returns false to reject the whole map, true to continue.
// Each policy also provides a NoAlloc overload (same contract, over pool
// pointers): `first_pair` is the base of the pair's Node* slot block,
// `existing_pair_index` is the matching earlier pair, and `new_key`/`new_value`
// are the newly decoded nodes.
struct RejectDuplicateKeys {
  static constexpr bool on_duplicate(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue) {
    return false;
  }
  static constexpr bool on_duplicate(NoAlloc::NodePool&, NoAlloc::Node** /*first_pair*/,
                                      std::size_t /*existing_pair_index*/, NoAlloc::Node* /*new_key*/,
                                      NoAlloc::Node* /*new_value*/) {
    return false;
  }
  // SAX overload: no tree exists, so the verdict is a plain reject. Keys are
  // compared by encoded bytes (see SaxRange).
  static constexpr bool on_duplicate() { return false; }
};
struct KeepFirstDuplicateKey {
  static constexpr bool on_duplicate(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue) {
    return true;  // first occurrence already in entries; drop this one
  }
  static constexpr bool on_duplicate(NoAlloc::NodePool&, NoAlloc::Node** /*first_pair*/,
                                      std::size_t /*existing_pair_index*/, NoAlloc::Node* /*new_key*/,
                                      NoAlloc::Node* /*new_value*/) {
    return true;  // first occurrence's pair is left exactly where it is; drop this one
  }
  static constexpr bool on_duplicate() { return true; }  // SAX: drop the new pair's events, emit nothing for it
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
  // Defined out of line after NoAlloc::Node is complete; GCC checks the
  // aggregate initialization eagerly.
  static constexpr bool on_duplicate(NoAlloc::NodePool& pool, NoAlloc::Node** first_pair,
                                      std::size_t existing_pair_index, NoAlloc::Node* new_key,
                                      NoAlloc::Node* new_value);
  // No SAX overload: keeping the last value would mean rewriting an
  // already-emitted event. Using this policy with SaxRange is a compile error
  // (see is_sax_duplicate_key_policy_v).
};

// Don't track keys. For the heap and NoAlloc decoders this behaves like
// KeepFirstDuplicateKey, since they scan for duplicates anyway. For SAX it skips
// the key-tracking buffer entirely (see SaxRange's MaxTrackedKeys).
struct NoDuplicateKeyCheck {
  static constexpr bool on_duplicate(std::vector<std::pair<CborValue, CborValue>>&, CborValue, CborValue) {
    return true;
  }
  static constexpr bool on_duplicate(NoAlloc::NodePool&, NoAlloc::Node** /*first_pair*/,
                                      std::size_t /*existing_pair_index*/, NoAlloc::Node* /*new_key*/,
                                      NoAlloc::Node* /*new_value*/) {
    return true;
  }
  // SAX: never called (tracking is skipped), but required by
  // is_sax_duplicate_key_policy_v.
  static constexpr bool on_duplicate() { return true; }
};

// SAX counterpart of is_duplicate_key_policy_v, pinning the 0-arg overload.
template <typename P, typename = void>
struct is_sax_duplicate_key_policy : std::false_type {};

template <typename P>
struct is_sax_duplicate_key_policy<P, std::void_t<decltype(static_cast<bool (*)()>(&P::on_duplicate))>>
    : std::true_type {};

template <typename P>
inline constexpr bool is_sax_duplicate_key_policy_v = is_sax_duplicate_key_policy<P>::value;


namespace detail {

// ---------------------------------------------------------------------------
// The one structural walk behind every tree-shaped decode and skip. It owns
// everything about well-formedness: headers, definite and indefinite forms,
// chunk checking, depth, truncation, and error offsets. What it produces is
// delegated to a Builder:
//
//   using Value;                      // result for one item
//   Value make_uint(uint64_t);
//   Value make_negint(NegativeInt);
//   Value make_simple(const DecodedSimple&);
//   Value make_string(StringKind, span<const uint8_t>);   // definite length
//   Value make_tag(uint64_t number, Value nested);
//   struct StringAcc { void append(span<const uint8_t>); Value finish(StringKind); };
//   struct ArrayAcc  { void push(Value); Value finish(); };
//   struct MapAcc    { bool add(Value key, Value value); Value finish(); };
//
// MapAcc::add returns false to reject the map (reported as DuplicateKey).
// Walking with a Builder that builds nothing is how skip_item measures an item.
template <std::size_t MaxDepth, typename Utf8Policy, typename Builder>
constexpr Expected<std::pair<typename Builder::Value, std::size_t>, Error> walk(std::span<const std::uint8_t> bytes,
                                                                               std::size_t depth = 0) {
  using Value = typename Builder::Value;
  using Result = std::pair<Value, std::size_t>;
  if (bytes.empty()) {
    return unexpected(Error{ErrorReason::TruncatedInput});
  }
  const std::uint8_t major_type = bytes[0] >> 5;

  switch (major_type) {
    case 0: {
      const auto u = decode_uint(bytes);
      if (!u) return unexpected(u.error());
      return Result{Builder::make_uint(u->value), u->bytes_consumed};
    }
    case 1: {
      const auto n = decode_negint(bytes);
      if (!n) return unexpected(n.error());
      return Result{Builder::make_negint(n->value), n->bytes_consumed};
    }
    case 2:
    case 3: {
      const auto kind = (major_type == 2) ? CborValue::StringKind::Bytes : CborValue::StringKind::Text;
      if (is_indefinite_or_break(bytes)) {
        typename Builder::StringAcc acc;
        std::size_t consumed = 1;
        while (true) {
          if (consumed >= bytes.size()) {
            return unexpected(Error{ErrorReason::TruncatedInput, consumed});
          }
          if (bytes[consumed] == 0xff) {
            ++consumed;
            break;
          }
          // Each chunk must itself be definite-length and of the same major type.
          const auto chunk = (major_type == 2) ? decode_byte_string(bytes.subspan(consumed))
                                                : decode_text_string<Utf8Policy>(bytes.subspan(consumed));
          if (!chunk) return unexpected(rebase_error(chunk.error(), consumed));
          acc.append(chunk->bytes);
          consumed += chunk->bytes_consumed;
        }
        return Result{acc.finish(kind), consumed};
      }
      const auto result = (major_type == 2) ? decode_byte_string(bytes) : decode_text_string<Utf8Policy>(bytes);
      if (!result) return unexpected(result.error());
      return Result{Builder::make_string(kind, result->bytes), result->bytes_consumed};
    }
    case 4: {
      if (depth >= MaxDepth) return unexpected(Error{ErrorReason::MaxDepthExceeded});
      typename Builder::ArrayAcc acc;
      std::size_t consumed = 1;
      if (is_indefinite_or_break(bytes)) {
        while (true) {
          if (consumed >= bytes.size()) return unexpected(Error{ErrorReason::TruncatedInput, consumed});
          if (bytes[consumed] == 0xff) {
            ++consumed;
            break;
          }
          auto item = walk<MaxDepth, Utf8Policy, Builder>(bytes.subspan(consumed), depth + 1);
          if (!item) return unexpected(rebase_error(item.error(), consumed));
          acc.push(std::move(item->first));
          consumed += item->second;
        }
      } else {
        const auto len = decode_length(bytes);
        if (!len) return unexpected(len.error());
        consumed = len->bytes_consumed;
        for (std::uint64_t i = 0; i < len->value; ++i) {
          if (consumed > bytes.size()) return unexpected(Error{ErrorReason::TruncatedPayload, consumed});
          auto item = walk<MaxDepth, Utf8Policy, Builder>(bytes.subspan(consumed), depth + 1);
          if (!item) return unexpected(rebase_error(item.error(), consumed));
          acc.push(std::move(item->first));
          consumed += item->second;
        }
      }
      return Result{acc.finish(), consumed};
    }
    case 5: {
      if (depth >= MaxDepth) return unexpected(Error{ErrorReason::MaxDepthExceeded});
      typename Builder::MapAcc acc;
      std::size_t consumed = 1;
      const bool indefinite = is_indefinite_or_break(bytes);
      std::optional<std::uint64_t> pair_count;
      if (!indefinite) {
        const auto len = decode_length(bytes);
        if (!len) return unexpected(len.error());
        consumed = len->bytes_consumed;
        pair_count = len->value;
      }
      std::uint64_t pairs_read = 0;
      while (indefinite ? true : pairs_read < *pair_count) {
        if (indefinite) {
          if (consumed >= bytes.size()) return unexpected(Error{ErrorReason::TruncatedInput, consumed});
          if (bytes[consumed] == 0xff) {
            ++consumed;
            break;
          }
        }
        auto key = walk<MaxDepth, Utf8Policy, Builder>(bytes.subspan(consumed), depth + 1);
        if (!key) return unexpected(rebase_error(key.error(), consumed));
        consumed += key->second;
        auto value = walk<MaxDepth, Utf8Policy, Builder>(bytes.subspan(consumed), depth + 1);
        if (!value) return unexpected(rebase_error(value.error(), consumed));
        consumed += value->second;
        if (!acc.add(std::move(key->first), std::move(value->first))) {
          return unexpected(Error{ErrorReason::DuplicateKey, consumed});
        }
        ++pairs_read;
      }
      return Result{acc.finish(), consumed};
    }
    case 6: {
      if (depth >= MaxDepth) return unexpected(Error{ErrorReason::MaxDepthExceeded});
      const auto len = decode_length(bytes);
      if (!len) return unexpected(len.error());
      auto nested = walk<MaxDepth, Utf8Policy, Builder>(bytes.subspan(len->bytes_consumed), depth + 1);
      if (!nested) return unexpected(rebase_error(nested.error(), len->bytes_consumed));
      return Result{Builder::make_tag(len->value, std::move(nested->first)), len->bytes_consumed + nested->second};
    }
    case 7: {
      const auto s = decode_simple(bytes);
      if (!s) return unexpected(s.error());
      return Result{Builder::make_simple(*s), s->bytes_consumed};
    }
    default:
      return unexpected(Error{ErrorReason::MalformedHeader});
  }
}

// Builder that builds nothing, for skip_item.
struct SkipBuilder {
  struct Value {};
  static constexpr Value make_uint(std::uint64_t) { return {}; }
  static constexpr Value make_negint(NegativeInt) { return {}; }
  static constexpr Value make_simple(const DecodedSimple&) { return {}; }
  static constexpr Value make_string(CborValue::StringKind, std::span<const std::uint8_t>) { return {}; }
  static constexpr Value make_tag(std::uint64_t, Value) { return {}; }
  struct StringAcc {
    constexpr void append(std::span<const std::uint8_t>) {}
    constexpr Value finish(CborValue::StringKind) { return {}; }
  };
  struct ArrayAcc {
    constexpr void push(Value) {}
    constexpr Value finish() { return {}; }
  };
  struct MapAcc {
    constexpr bool add(Value, Value) { return true; }
    constexpr Value finish() { return {}; }
  };
};

// Builder producing CborValue trees. Duplicate keys are resolved by KeyPolicy.
template <typename KeyPolicy>
struct HeapBuilder {
  static_assert(is_duplicate_key_policy_v<KeyPolicy>,
                "KeyPolicy must provide static bool on_duplicate(vector<pair<CborValue,CborValue>>&, "
                "CborValue, CborValue)");
  using Value = CborValue;
  static constexpr Value make_uint(std::uint64_t v) { return CborValue(v); }
  static constexpr Value make_negint(NegativeInt v) { return CborValue(v); }
  static constexpr Value make_string(CborValue::StringKind kind, std::span<const std::uint8_t> content) {
    return CborValue(CborValue::OwnedString{.kind = kind, .content = {content.begin(), content.end()}});
  }
  static constexpr Value make_simple(const DecodedSimple& s) {
    using K = DecodedSimple::Kind;
    switch (s.kind) {
      case K::False:
        return CborValue(false);
      case K::True:
        return CborValue(true);
      case K::Null:
        return CborValue(nullptr);
      case K::Undefined:
        return CborValue(std::monostate{});
      case K::Float:
        return CborValue(s.as_double);
      case K::Simple:
        break;
    }
    return CborValue(CborValue::SimpleValue{.number = s.simple_value});
  }
  static constexpr Value make_tag(std::uint64_t number, Value nested) {
    if (number == 2 || number == 3) {
      if (const auto* bytes_value = std::get_if<CborValue::OwnedString>(&nested.data);
          bytes_value && bytes_value->kind == CborValue::StringKind::Bytes) {
        return CborValue(CborValue::BigNum{.negative = (number == 3), .magnitude = bytes_value->content});
      }
    }
    std::vector<CborValue> wrapped;
    wrapped.push_back(std::move(nested));
    return CborValue(CborValue::Tag{.number = number, .value = std::move(wrapped)});
  }
  struct StringAcc {
    std::vector<std::uint8_t> content;
    constexpr void append(std::span<const std::uint8_t> chunk) { content.insert(content.end(), chunk.begin(), chunk.end()); }
    constexpr Value finish(CborValue::StringKind kind) {
      return CborValue(CborValue::OwnedString{.kind = kind, .content = std::move(content)});
    }
  };
  struct ArrayAcc {
    std::vector<CborValue> items;
    constexpr void push(Value v) { items.push_back(std::move(v)); }
    constexpr Value finish() { return CborValue(std::move(items)); }
  };
  struct MapAcc {
    std::vector<std::pair<CborValue, CborValue>> entries;
    constexpr bool add(Value key, Value value) {
      for (const auto& entry : entries) {
        if (entry.first.data == key.data) {
          return KeyPolicy::on_duplicate(entries, std::move(key), std::move(value));
        }
      }
      entries.emplace_back(std::move(key), std::move(value));
      return true;
    }
    constexpr Value finish() { return CborValue(std::move(entries)); }
  };
};

}  // namespace detail

// Measures one CBOR item (definite or indefinite) without building a value, and
// checks it exactly as decode_value does (minus UTF-8 and duplicate-key policy).
// The no-alloc decoder uses it to pre-count indefinite containers, and the SAX
// decoder to skip items.
template <std::size_t MaxDepth = 32>
constexpr Expected<std::size_t, Error> skip_item(std::span<const std::uint8_t> bytes, std::size_t depth = 0) {
  const auto result = detail::walk<MaxDepth, NoUtf8Validation, detail::SkipBuilder>(bytes, depth);
  if (!result) return unexpected(result.error());
  return result->second;
}

// The recursive decoder behind every major type. MaxDepth caps container nesting,
// the most common hazard across surveyed CBOR implementations (QCBOR, tinycbor,
// several CVEs). Exceeding it is an error, not a crash.
template <std::size_t MaxDepth = 32, typename Utf8Policy = NoUtf8Validation, typename KeyPolicy = RejectDuplicateKeys>
constexpr Expected<std::pair<CborValue, std::size_t>, Error> decode_value(std::span<const std::uint8_t> bytes,
                                                                           std::size_t depth = 0) {
  return detail::walk<MaxDepth, Utf8Policy, detail::HeapBuilder<KeyPolicy>>(bytes, depth);
}

// ---------------------------------------------------------------------------
// Encoding. Always emits the shortest (preferred) form, so output can be compared
// byte for byte against RFC 8949 Appendix A.

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

// std::isnan/isinf are not constexpr in C++20, and some libc++ releases (Clang
// 19-21, emscripten) lack it even in C++23. Plain comparisons work everywhere.
constexpr bool is_nan(double value) { return value != value; }
constexpr bool is_inf(double value) {
  return value == std::numeric_limits<double>::infinity() || value == -std::numeric_limits<double>::infinity();
}

constexpr bool bit_identical_double(double a, double b) {
  if (is_nan(a) || is_nan(b)) {
    return is_nan(a) && is_nan(b);
  }
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Derives a candidate half-float and accepts it only if decode_half_float
// round-trips it exactly.
constexpr std::optional<std::uint16_t> try_encode_half_float(double value) {
  std::uint16_t candidate;
  if (is_nan(value)) {
    candidate = 0x7e00;  // canonical NaN
  } else {
    const std::uint64_t bits64 = std::bit_cast<std::uint64_t>(value);
    // uint64_t so `sign << 15` is not promoted to signed int.
    const std::uint64_t sign = bits64 >> 63;
    if (value == 0.0) {
      candidate = static_cast<std::uint16_t>(sign << 15);
    } else if (is_inf(value)) {
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
  if (is_nan(value)) {
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
  const std::uint64_t d = detail::is_nan(value) ? 0x7ff8000000000000ull : std::bit_cast<std::uint64_t>(value);
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
