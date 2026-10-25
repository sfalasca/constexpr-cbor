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

// AsciiOnlyValidation is a different thing than the three UTF-8 policies
// above, not a fourth strictness level of the same thing: ASCII is a
// subset of UTF-8, so every one of the other policies accepts "IETF", but
// a perfectly valid, non-overlong 2-byte UTF-8 sequence ("ü") that
// CompleteUtf8Validation accepts still fails this one.
consteval bool test_ascii_only_policy_rejects_valid_non_ascii_utf8() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_ietf[] = {0x64, 0x49, 0x45, 0x54, 0x46};
  constexpr std::uint8_t vec_u_umlaut[] = {0x62, 0xc3, 0xbc};
  expect_eq<decode_text_string<AsciiOnlyValidation>(vec_ietf).has_value(), true>();
  expect_eq<decode_text_string<CompleteUtf8Validation>(vec_u_umlaut).has_value(), true>();
  expect_eq<decode_text_string<AsciiOnlyValidation>(vec_u_umlaut).has_value(), false>();
  return true;
}
static_assert(test_ascii_only_policy_rejects_valid_non_ascii_utf8());

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

// From cbor-wg/cbor-test-vectors (tests/rfc8949/good.edn): two Unicode
// edge cases beyond RFC 8949's own umlaut/water/supplementary-plane
// examples. "string: BOM" embeds a byte-order mark (U+FEFF) as ordinary
// payload content, not a stream prefix -- a decoder that special-cases
// BOM detection anywhere would mishandle this. "string: combining" is "u"
// (U+0075) followed by a combining diaeresis (U+0308), which renders
// identically to the precomposed "ü" this file already tests but is
// a completely different byte sequence -- canonical-equivalence is a
// semantic question this library deliberately doesn't resolve (bytes are
// bytes), which this test documents rather than silently assumes.
consteval bool test_decode_text_string_matches_cbor_test_vectors_unicode_cases() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_bom[] = {0x66, 0xef, 0xbb, 0xbf, 0x42, 0x4f, 0x4d};
  constexpr std::uint8_t vec_combining[] = {0x63, 0x75, 0xcc, 0x88};
  expect_eq<decode_text_string(vec_bom)->bytes.size(), std::size_t{6}>();
  expect_eq<decode_text_string(vec_combining)->bytes.size(), std::size_t{3}>();
  expect_eq<decode_text_string(vec_combining)->bytes[0], std::uint8_t{0x75}>();
  return true;
}
static_assert(test_decode_text_string_matches_cbor_test_vectors_unicode_cases());

int main() { return 0; }
