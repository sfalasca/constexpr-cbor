#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: false -> 0xf4, true -> 0xf5, null -> 0xf6, undefined -> 0xf7.
consteval bool test_decode_simple_matches_boolean_null_undefined_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_false[] = {0xf4};
  constexpr std::uint8_t vec_true[] = {0xf5};
  constexpr std::uint8_t vec_null[] = {0xf6};
  constexpr std::uint8_t vec_undefined[] = {0xf7};

  expect_eq<decode_simple(vec_false)->kind, DecodedSimple::Kind::False>();
  expect_eq<decode_simple(vec_true)->kind, DecodedSimple::Kind::True>();
  expect_eq<decode_simple(vec_null)->kind, DecodedSimple::Kind::Null>();
  expect_eq<decode_simple(vec_undefined)->kind, DecodedSimple::Kind::Undefined>();
  return true;
}
static_assert(test_decode_simple_matches_boolean_null_undefined_vectors());

// RFC 8949 Appendix A half-float vectors: 0.0 -> 0xf90000, -0.0 -> 0xf98000,
// 1.0 -> 0xf93c00, 1.5 -> 0xf93e00, 65504.0 (largest normal) -> 0xf97bff,
// 5.960464477539063e-8 (smallest subnormal) -> 0xf90001, Infinity -> 0xf97c00,
// -Infinity -> 0xf9fc00.
consteval bool test_decode_simple_matches_official_half_float_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_zero[] = {0xf9, 0x00, 0x00};
  constexpr std::uint8_t vec_neg_zero[] = {0xf9, 0x80, 0x00};
  constexpr std::uint8_t vec_one[] = {0xf9, 0x3c, 0x00};
  constexpr std::uint8_t vec_1_5[] = {0xf9, 0x3e, 0x00};
  constexpr std::uint8_t vec_largest_normal[] = {0xf9, 0x7b, 0xff};
  constexpr std::uint8_t vec_smallest_subnormal[] = {0xf9, 0x00, 0x01};
  constexpr std::uint8_t vec_infinity[] = {0xf9, 0x7c, 0x00};
  constexpr std::uint8_t vec_neg_infinity[] = {0xf9, 0xfc, 0x00};
  constexpr std::uint8_t vec_another_subnormal[] = {0xf9, 0x04, 0x00};  // 0.00006103515625
  constexpr std::uint8_t vec_neg_4[] = {0xf9, 0xc4, 0x00};              // -4.0
  constexpr std::uint8_t vec_nan[] = {0xf9, 0x7e, 0x00};

  expect_eq<decode_simple(vec_zero)->as_double, 0.0>();
  expect_eq<decode_simple(vec_neg_zero)->as_double, -0.0>();
  expect_eq<decode_simple(vec_one)->as_double, 1.0>();
  expect_eq<decode_simple(vec_1_5)->as_double, 1.5>();
  expect_eq<decode_simple(vec_largest_normal)->as_double, 65504.0>();
  expect_eq<decode_simple(vec_smallest_subnormal)->as_double, 5.960464477539063e-8>();
  expect_eq<decode_simple(vec_another_subnormal)->as_double, 0.00006103515625>();
  expect_eq<decode_simple(vec_neg_4)->as_double, -4.0>();
  expect_eq<decode_simple(vec_infinity)->as_double, std::numeric_limits<double>::infinity()>();
  expect_eq<decode_simple(vec_neg_infinity)->as_double, -std::numeric_limits<double>::infinity()>();
  expect_eq<std::isnan(decode_simple(vec_nan)->as_double), true>();
  return true;
}
static_assert(test_decode_simple_matches_official_half_float_vectors());

// RFC 8949 Appendix A single-float vectors: 100000.0 -> 0xfa47c35000,
// 3.4028234663852886e+38 (largest finite single) -> 0xfa7f7fffff,
// Infinity -> 0xfa7f800000, -Infinity -> 0xfaff800000.
consteval bool test_decode_simple_matches_official_single_float_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_100000[] = {0xfa, 0x47, 0xc3, 0x50, 0x00};
  constexpr std::uint8_t vec_largest_finite[] = {0xfa, 0x7f, 0x7f, 0xff, 0xff};
  constexpr std::uint8_t vec_infinity[] = {0xfa, 0x7f, 0x80, 0x00, 0x00};
  constexpr std::uint8_t vec_neg_infinity[] = {0xfa, 0xff, 0x80, 0x00, 0x00};
  constexpr std::uint8_t vec_nan[] = {0xfa, 0x7f, 0xc0, 0x00, 0x00};

  expect_eq<decode_simple(vec_100000)->as_double, 100000.0>();
  expect_eq<decode_simple(vec_largest_finite)->as_double, 3.4028234663852886e+38>();
  expect_eq<decode_simple(vec_infinity)->as_double, std::numeric_limits<double>::infinity()>();
  expect_eq<decode_simple(vec_neg_infinity)->as_double, -std::numeric_limits<double>::infinity()>();
  expect_eq<std::isnan(decode_simple(vec_nan)->as_double), true>();
  return true;
}
static_assert(test_decode_simple_matches_official_single_float_vectors());

// RFC 8949 Appendix A double-float vectors: 1.1 -> 0xfb3ff199999999999a,
// 1.0e+300 -> 0xfb7e37e43c8800759c, -4.1 -> 0xfbc010666666666666,
// Infinity -> 0xfb7ff0000000000000, -Infinity -> 0xfbfff0000000000000.
consteval bool test_decode_simple_matches_official_double_float_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_1_1[] = {0xfb, 0x3f, 0xf1, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9a};
  constexpr std::uint8_t vec_1e300[] = {0xfb, 0x7e, 0x37, 0xe4, 0x3c, 0x88, 0x00, 0x75, 0x9c};
  constexpr std::uint8_t vec_neg_4_1[] = {0xfb, 0xc0, 0x10, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
  constexpr std::uint8_t vec_infinity[] = {0xfb, 0x7f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  constexpr std::uint8_t vec_neg_infinity[] = {0xfb, 0xff, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  constexpr std::uint8_t vec_nan[] = {0xfb, 0x7f, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  expect_eq<decode_simple(vec_1_1)->as_double, 1.1>();
  expect_eq<decode_simple(vec_1e300)->as_double, 1.0e+300>();
  expect_eq<decode_simple(vec_neg_4_1)->as_double, -4.1>();
  expect_eq<decode_simple(vec_infinity)->as_double, std::numeric_limits<double>::infinity()>();
  expect_eq<decode_simple(vec_neg_infinity)->as_double, -std::numeric_limits<double>::infinity()>();
  expect_eq<std::isnan(decode_simple(vec_nan)->as_double), true>();
  return true;
}
static_assert(test_decode_simple_matches_official_double_float_vectors());

// RFC 8949 Appendix A: simple(16) -> 0xf0, simple(255) -> 0xf8ff. Additional
// info 0-19 is "unassigned" but still well-formed and decodable; 24 is the
// one-byte form for values 32-255.
consteval bool test_decode_simple_matches_official_simple_value_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_16[] = {0xf0};
  constexpr std::uint8_t vec_255[] = {0xf8, 0xff};
  expect_eq<decode_simple(vec_16)->kind, DecodedSimple::Kind::Simple>();
  expect_eq<decode_simple(vec_16)->simple_value, std::uint8_t{16}>();
  expect_eq<decode_simple(vec_255)->simple_value, std::uint8_t{255}>();
  expect_eq<decode_simple(vec_255)->bytes_consumed, std::size_t{2}>();
  return true;
}
static_assert(test_decode_simple_matches_official_simple_value_vectors());

consteval bool test_decode_simple_rejects_reserved_and_break() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_28[] = {0xfc};  // additional info 28, reserved
  constexpr std::uint8_t vec_31[] = {0xff};  // additional info 31, "break"
  expect_eq<decode_simple(vec_28).has_value(), false>();
  expect_eq<decode_simple(vec_31).has_value(), false>();
  return true;
}
static_assert(test_decode_simple_rejects_reserved_and_break());

consteval bool test_encode_simple_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<std::uint8_t, 1> expected_16{0xf0};
  constexpr std::array<std::uint8_t, 2> expected_255{0xf8, 0xff};
  const CborValue v16{.data = CborValue::SimpleValue{.number = 16}};
  const CborValue v255{.data = CborValue::SimpleValue{.number = 255}};
  const auto e16 = encode_value(v16);
  const auto e255 = encode_value(v255);
  expect_eq(std::span<const std::uint8_t>(e16), std::span<const std::uint8_t>(expected_16));
  expect_eq(std::span<const std::uint8_t>(e255), std::span<const std::uint8_t>(expected_255));
  return true;
}
static_assert(test_encode_simple_matches_official_vectors());

consteval bool test_decode_simple_rejects_wrong_major_type() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t wrong[] = {0x00};
  expect_eq<decode_simple(wrong).has_value(), false>();
  return true;
}
static_assert(test_decode_simple_rejects_wrong_major_type());

// Canonical encoding: every finite value that fits half-precision exactly
// must encode there, not single or double, even though RFC 8949's own
// appendix lists Infinity/NaN at all three widths as valid (just not all
// preferred) encodings of the same value.
consteval bool test_encode_float_always_picks_the_shortest_exact_width() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<std::uint8_t, 3> expected_zero{0xf9, 0x00, 0x00};
  constexpr std::array<std::uint8_t, 3> expected_infinity{0xf9, 0x7c, 0x00};
  constexpr std::array<std::uint8_t, 5> expected_100000{0xfa, 0x47, 0xc3, 0x50, 0x00};
  constexpr std::array<std::uint8_t, 9> expected_1_1{0xfb, 0x3f, 0xf1, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9a};

  const auto e1 = encode_float(0.0);
  const auto e2 = encode_float(std::numeric_limits<double>::infinity());
  const auto e3 = encode_float(100000.0);
  const auto e4 = encode_float(1.1);
  expect_eq(std::span<const std::uint8_t>(e1), std::span<const std::uint8_t>(expected_zero));
  expect_eq(std::span<const std::uint8_t>(e2), std::span<const std::uint8_t>(expected_infinity));
  expect_eq(std::span<const std::uint8_t>(e3), std::span<const std::uint8_t>(expected_100000));
  expect_eq(std::span<const std::uint8_t>(e4), std::span<const std::uint8_t>(expected_1_1));
  return true;
}
static_assert(test_encode_float_always_picks_the_shortest_exact_width());

int main() { return 0; }
