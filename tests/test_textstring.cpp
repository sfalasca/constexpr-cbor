#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: "" -> 0x60, "a" -> 0x6161, "IETF" -> 0x6449455446,
// "\"\\" -> 0x62225c, "ü" -> 0x62c3bc, "水" -> 0x63e6b0b4,
// "𐅑" (U+10151, a valid 4-byte UTF-8 sequence) -> 0x64f0908591.
consteval bool test_decode_text_string_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_empty[] = {0x60};
  constexpr std::uint8_t vec_a[] = {0x61, 0x61};
  constexpr std::uint8_t vec_ietf[] = {0x64, 0x49, 0x45, 0x54, 0x46};
  constexpr std::uint8_t vec_escapes[] = {0x62, 0x22, 0x5c};
  constexpr std::uint8_t vec_u_umlaut[] = {0x62, 0xc3, 0xbc};
  constexpr std::uint8_t vec_water[] = {0x63, 0xe6, 0xb0, 0xb4};
  constexpr std::uint8_t vec_supplementary[] = {0x64, 0xf0, 0x90, 0x85, 0x91};

  expect_eq<decode_text_string(vec_empty)->bytes.size(), std::size_t{0}>();
  expect_eq<decode_text_string(vec_a)->bytes_consumed, std::size_t{2}>();
  expect_eq<decode_text_string(vec_ietf)->bytes.size(), std::size_t{4}>();
  expect_eq<decode_text_string(vec_escapes)->bytes.size(), std::size_t{2}>();
  expect_eq<decode_text_string(vec_u_umlaut)->bytes.size(), std::size_t{2}>();
  expect_eq<decode_text_string(vec_water)->bytes.size(), std::size_t{3}>();
  expect_eq<decode_text_string(vec_supplementary)->bytes.size(), std::size_t{4}>();
  return true;
}
static_assert(test_decode_text_string_matches_official_vectors());

consteval bool test_decode_text_string_rejects_wrong_major_type() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t wrong[] = {0x40};  // major type 2 (byte string)
  expect_eq<decode_text_string(wrong).has_value(), false>();
  return true;
}
static_assert(test_decode_text_string_rejects_wrong_major_type());

// The three UTF-8 policies, against the same three adversarial inputs, so
// they actually diverge visibly rather than just satisfying the trait.
consteval bool test_utf8_policies_diverge_on_overlong_encoding() {
  using namespace CompileTimeUnitTesting;
  // RFC 8949's own example of well-formed-but-invalid CBOR: an overlong
  // 2-byte encoding of ASCII '.', which only needs one byte.
  constexpr std::uint8_t vec_overlong_dot[] = {0x62, 0xc0, 0xae};
  expect_eq<decode_text_string<NoUtf8Validation>(vec_overlong_dot).has_value(), true>();
  expect_eq<decode_text_string<RoughUtf8Validation>(vec_overlong_dot).has_value(), true>();
  expect_eq<decode_text_string<CompleteUtf8Validation>(vec_overlong_dot).has_value(), false>();
  return true;
}
static_assert(test_utf8_policies_diverge_on_overlong_encoding());

consteval bool test_utf8_policies_diverge_on_lone_continuation_byte() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_lone_continuation[] = {0x61, 0x80};  // a stray 10xxxxxx byte, no lead byte
  expect_eq<decode_text_string<NoUtf8Validation>(vec_lone_continuation).has_value(), true>();
  expect_eq<decode_text_string<RoughUtf8Validation>(vec_lone_continuation).has_value(), false>();
  expect_eq<decode_text_string<CompleteUtf8Validation>(vec_lone_continuation).has_value(), false>();
  return true;
}
static_assert(test_utf8_policies_diverge_on_lone_continuation_byte());

consteval bool test_utf8_policies_diverge_on_surrogate_half() {
  using namespace CompileTimeUnitTesting;
  // U+D800 encoded directly as 3-byte UTF-8 (0xed 0xa0 0x80): structurally
  // well-formed (right continuation-byte shape), so Rough accepts it;
  // Complete must reject it (surrogate halves are never valid UTF-8 per
  // RFC 3629).
  constexpr std::uint8_t vec_surrogate[] = {0x63, 0xed, 0xa0, 0x80};
  expect_eq<decode_text_string<NoUtf8Validation>(vec_surrogate).has_value(), true>();
  expect_eq<decode_text_string<RoughUtf8Validation>(vec_surrogate).has_value(), true>();
  expect_eq<decode_text_string<CompleteUtf8Validation>(vec_surrogate).has_value(), false>();
  return true;
}
static_assert(test_utf8_policies_diverge_on_surrogate_half());

consteval bool test_utf8_policies_accept_the_official_vectors() {
  using namespace CompileTimeUnitTesting;
  // Every official vector above is genuinely valid UTF-8; all three
  // policies must accept all of them.
  constexpr std::uint8_t vec_u_umlaut[] = {0x62, 0xc3, 0xbc};
  constexpr std::uint8_t vec_supplementary[] = {0x64, 0xf0, 0x90, 0x85, 0x91};
  expect_eq<decode_text_string<RoughUtf8Validation>(vec_u_umlaut).has_value(), true>();
  expect_eq<decode_text_string<CompleteUtf8Validation>(vec_u_umlaut).has_value(), true>();
  expect_eq<decode_text_string<RoughUtf8Validation>(vec_supplementary).has_value(), true>();
  expect_eq<decode_text_string<CompleteUtf8Validation>(vec_supplementary).has_value(), true>();
  return true;
}
static_assert(test_utf8_policies_accept_the_official_vectors());

consteval bool test_encode_text_string_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<char, 4> ietf{'I', 'E', 'T', 'F'};
  constexpr std::array<std::uint8_t, 5> expected{0x64, 0x49, 0x45, 0x54, 0x46};
  std::array<std::uint8_t, 4> ietf_bytes{};
  for (std::size_t i = 0; i < 4; ++i) ietf_bytes[i] = static_cast<std::uint8_t>(ietf[i]);
  const auto encoded = encode_text_string(ietf_bytes);
  expect_eq(std::span<const std::uint8_t>(encoded), std::span<const std::uint8_t>(expected));
  return true;
}
static_assert(test_encode_text_string_matches_official_vectors());

consteval bool test_textstring_roundtrips() {
  constexpr std::uint8_t vec[] = {0x63, 0xe6, 0xb0, 0xb4};  // "水"
  const auto decoded = decode_text_string(vec);
  if (!decoded) return false;
  const auto reencoded = encode_text_string(decoded->bytes);
  return std::equal(reencoded.begin(), reencoded.end(), vec);
}
static_assert(test_textstring_roundtrips());

int main() { return 0; }
