#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: 1(1363896240) -> 0xc11a514b67b0,
// 23(h'01020304') -> 0xd74401020304, 32("http://www.example.com") ->
// 0xd82076687474703a2f2f7777772e6578616d706c652e636f6d.
consteval bool test_decode_tag_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_epoch[] = {0xc1, 0x1a, 0x51, 0x4b, 0x67, 0xb0};
  constexpr std::uint8_t vec_bignum_tag[] = {0xd7, 0x44, 0x01, 0x02, 0x03, 0x04};

  expect_eq<decode_value(vec_epoch)->second, std::size_t{6}>();
  expect_eq<std::get<CborValue::Tag>(decode_value(vec_epoch)->first.data).number, std::uint64_t{1}>();
  expect_eq<std::get<std::uint64_t>(
                std::get<CborValue::Tag>(decode_value(vec_epoch)->first.data).value.front().data),
            std::uint64_t{1363896240}>();
  expect_eq<std::get<CborValue::Tag>(decode_value(vec_bignum_tag)->first.data).number, std::uint64_t{23}>();
  return true;
}
static_assert(test_decode_tag_matches_official_vectors());

constexpr bool test_decode_tag_wraps_exactly_one_item() {
  constexpr std::uint8_t vec[] = {0xc1, 0x1a, 0x51, 0x4b, 0x67, 0xb0};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& tag = std::get<CborValue::Tag>(d->first.data);
  return tag.value.size() == 1;
}
static_assert(test_decode_tag_wraps_exactly_one_item());

// A fractional-second timestamp tag: 1(1363896240.5) -> 0xc1fb41d452d9ec200000
constexpr bool test_decode_tag_wraps_a_float() {
  constexpr std::uint8_t vec[] = {0xc1, 0xfb, 0x41, 0xd4, 0x52, 0xd9, 0xec, 0x20, 0x00, 0x00};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& tag = std::get<CborValue::Tag>(d->first.data);
  return tag.number == 1 && std::get<double>(tag.value.front().data) == 1363896240.5;
}
static_assert(test_decode_tag_wraps_a_float());

consteval bool test_decode_tag_refuses_truncated_input() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t truncated[] = {0xc1};  // tag number present, nested item missing entirely
  expect_eq<decode_value(truncated).has_value(), false>();
  return true;
}
static_assert(test_decode_tag_refuses_truncated_input());

consteval bool test_encode_tag_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<std::uint8_t, 6> expected{0xc1, 0x1a, 0x51, 0x4b, 0x67, 0xb0};
  const CborValue value{.data = CborValue::Tag{
                             .number = 1,
                             .value = {CborValue{.data = std::uint64_t{1363896240}}},
                         }};
  const auto encoded = encode_value(value);
  expect_eq(std::span<const std::uint8_t>(encoded), std::span<const std::uint8_t>(expected));
  return true;
}
static_assert(test_encode_tag_matches_official_vectors());

constexpr bool test_tag_roundtrips() {
  constexpr std::uint8_t vec[] = {0xd7, 0x44, 0x01, 0x02, 0x03, 0x04};  // 23(h'01020304')
  const auto decoded = decode_value(vec);
  if (!decoded) return false;
  const auto reencoded = encode_value(decoded->first);
  return std::equal(reencoded.begin(), reencoded.end(), vec);
}
static_assert(test_tag_roundtrips());

int main() { return 0; }
