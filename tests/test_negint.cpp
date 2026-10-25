#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: -1 -> 0x20, -10 -> 0x29, -100 -> 0x3863, -1000 -> 0x3903e7.
consteval bool test_decode_negint_matches_small_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_neg1[] = {0x20};
  constexpr std::uint8_t vec_neg10[] = {0x29};
  constexpr std::uint8_t vec_neg100[] = {0x38, 0x63};
  constexpr std::uint8_t vec_neg1000[] = {0x39, 0x03, 0xe7};

  expect_eq<decode_negint(vec_neg1)->value.to_int64().value(), std::int64_t{-1}>();
  expect_eq<decode_negint(vec_neg10)->value.to_int64().value(), std::int64_t{-10}>();
  expect_eq<decode_negint(vec_neg100)->value.to_int64().value(), std::int64_t{-100}>();
  expect_eq<decode_negint(vec_neg1000)->value.to_int64().value(), std::int64_t{-1000}>();
  return true;
}
static_assert(test_decode_negint_matches_small_vectors());

// The boundary tinycbor issue #347 is open about: the true value can be as
// negative as -2^64, which does not fit int64_t (min -2^63). RFC 8949
// Appendix A: -18446744073709551616 -> 0x3bffffffffffffffff.
consteval bool test_decode_negint_handles_boundary_out_of_int64_range() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_min[] = {0x3b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  expect_eq<decode_negint(vec_min)->value.n, std::numeric_limits<std::uint64_t>::max()>();
  expect_eq<decode_negint(vec_min)->value.to_int64().has_value(), false>();
  return true;
}
static_assert(test_decode_negint_handles_boundary_out_of_int64_range());

// -18446744073709551617 -> 0xc349010000000000000000 is a tag-2 bignum, out of
// scope for decode_negint itself (it needs a value even further negative than
// major type 1 alone can express) -- not tested here, belongs to the tag
// decoder's own test file once bignum tags are in scope.

consteval bool test_decode_negint_rejects_wrong_major_type() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t wrong[] = {0x00};  // major type 0
  expect_eq<decode_negint(wrong).has_value(), false>();
  return true;
}
static_assert(test_decode_negint_rejects_wrong_major_type());

consteval bool test_encode_negint_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<std::uint8_t, 1> expected_neg1{0x20};
  constexpr std::array<std::uint8_t, 9> expected_min{0x3b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  const auto e1 = encode_negint(NegativeInt{.n = 0});
  const auto e2 = encode_negint(NegativeInt{.n = std::numeric_limits<std::uint64_t>::max()});
  expect_eq(std::span<const std::uint8_t>(e1), std::span<const std::uint8_t>(expected_neg1));
  expect_eq(std::span<const std::uint8_t>(e2), std::span<const std::uint8_t>(expected_min));
  return true;
}
static_assert(test_encode_negint_matches_official_vectors());

consteval bool test_negint_roundtrips_at_the_boundary() {
  constexpr std::uint8_t vec_min[] = {0x3b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  const auto decoded = decode_negint(vec_min);
  if (!decoded) return false;
  const auto reencoded = encode_negint(decoded->value);
  return std::equal(reencoded.begin(), reencoded.end(), vec_min);
}
static_assert(test_negint_roundtrips_at_the_boundary());

int main() { return 0; }
