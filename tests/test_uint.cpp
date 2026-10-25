#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: 0 -> 0x00, 1 -> 0x01, 10 -> 0x0a, 23 -> 0x17.
consteval bool test_decode_uint_matches_small_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr DecodedUint sentinel{.value = 999, .bytes_consumed = 999};
  constexpr std::uint8_t vec_0[] = {0x00};
  constexpr std::uint8_t vec_1[] = {0x01};
  constexpr std::uint8_t vec_10[] = {0x0a};
  constexpr std::uint8_t vec_23[] = {0x17};

  expect_eq<decode_uint(vec_0).value_or(sentinel).value, std::uint64_t{0}>();
  expect_eq<decode_uint(vec_1).value_or(sentinel).value, std::uint64_t{1}>();
  expect_eq<decode_uint(vec_10).value_or(sentinel).value, std::uint64_t{10}>();
  expect_eq<decode_uint(vec_23).value_or(sentinel).value, std::uint64_t{23}>();
  return true;
}
static_assert(test_decode_uint_matches_small_vectors());

// RFC 8949 Appendix A: 24 -> 0x1818, 25 -> 0x1819, 100 -> 0x1864, 1000 -> 0x1903e8,
// 1000000 -> 0x1a000f4240, 1000000000000 -> 0x1b000000e8d4a51000,
// 18446744073709551615 -> 0x1bffffffffffffffff (UINT64_MAX).
consteval bool test_decode_uint_matches_extended_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr DecodedUint sentinel{.value = 999, .bytes_consumed = 999};
  constexpr std::uint8_t vec_24[] = {0x18, 0x18};
  constexpr std::uint8_t vec_100[] = {0x18, 0x64};
  constexpr std::uint8_t vec_1000[] = {0x19, 0x03, 0xe8};
  constexpr std::uint8_t vec_1000000[] = {0x1a, 0x00, 0x0f, 0x42, 0x40};
  constexpr std::uint8_t vec_1e12[] = {0x1b, 0x00, 0x00, 0x00, 0xe8, 0xd4, 0xa5, 0x10, 0x00};
  constexpr std::uint8_t vec_max_u64[] = {0x1b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

  expect_eq<decode_uint(vec_24).value_or(sentinel).value, std::uint64_t{24}>();
  expect_eq<decode_uint(vec_100).value_or(sentinel).value, std::uint64_t{100}>();
  expect_eq<decode_uint(vec_1000).value_or(sentinel).value, std::uint64_t{1000}>();
  expect_eq<decode_uint(vec_1000000).value_or(sentinel).value, std::uint64_t{1000000}>();
  expect_eq<decode_uint(vec_1e12).value_or(sentinel).value, std::uint64_t{1000000000000ull}>();
  expect_eq<decode_uint(vec_max_u64).value_or(sentinel).value, std::numeric_limits<std::uint64_t>::max()>();
  return true;
}
static_assert(test_decode_uint_matches_extended_vectors());

consteval bool test_decode_uint_rejects_wrong_major_type() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t lowest_wrong[] = {0x20};
  constexpr std::uint8_t highest_wrong[] = {0xe0};
  expect_eq<decode_uint(lowest_wrong).has_value(), false>();
  expect_eq<decode_uint(highest_wrong).has_value(), false>();
  return true;
}
static_assert(test_decode_uint_rejects_wrong_major_type());

consteval bool test_decode_uint_ignores_trailing_bytes() {
  using namespace CompileTimeUnitTesting;
  constexpr DecodedUint sentinel{.value = 999, .bytes_consumed = 999};
  constexpr std::uint8_t with_trailing[] = {0x17, 0xff, 0xff};
  expect_eq<decode_uint(with_trailing).value_or(sentinel).value, std::uint64_t{23}>();
  expect_eq<decode_uint(with_trailing).value_or(sentinel).bytes_consumed, std::size_t{1}>();
  return true;
}
static_assert(test_decode_uint_ignores_trailing_bytes());

// Encoder: must always emit the shortest form.
consteval bool test_encode_uint_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<std::uint8_t, 1> expected_23{0x17};
  constexpr std::array<std::uint8_t, 9> expected_1e12{0x1b, 0x00, 0x00, 0x00, 0xe8, 0xd4, 0xa5, 0x10, 0x00};
  const auto e1 = encode_uint(23);
  const auto e2 = encode_uint(1000000000000ull);
  expect_eq(std::span<const std::uint8_t>(e1), std::span<const std::uint8_t>(expected_23));
  expect_eq(std::span<const std::uint8_t>(e2), std::span<const std::uint8_t>(expected_1e12));
  return true;
}
static_assert(test_encode_uint_matches_official_vectors());

// Full decode -> encode round-trip for every vector above.
consteval bool test_uint_roundtrips() {
  constexpr std::uint8_t vec_1000000[] = {0x1a, 0x00, 0x0f, 0x42, 0x40};
  constexpr std::uint8_t vec_max_u64[] = {0x1b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  const auto d1 = decode_uint(vec_1000000);
  const auto d2 = decode_uint(vec_max_u64);
  if (!d1 || !d2) return false;
  const auto e1 = encode_uint(d1->value);
  const auto e2 = encode_uint(d2->value);
  return std::equal(e1.begin(), e1.end(), vec_1000000) && std::equal(e2.begin(), e2.end(), vec_max_u64);
}
static_assert(test_uint_roundtrips());

int main() { return 0; }
