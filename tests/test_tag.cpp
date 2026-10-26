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
  const CborValue value(CborValue::Tag{
      .number = 1,
      .value = {CborValue(std::uint64_t{1363896240})},
  });
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

// RFC 8949 Appendix A: 18446744073709551616 (2^64, past uint64's ceiling) ->
// 0xc249010000000000000000 (tag 2, positive bignum); -18446744073709551617
// -> 0xc349010000000000000000 (tag 3, same magnitude, negative bignum).
// Major types 0/1 top out at 64-bit magnitude; this is CBOR's actual
// mechanism for integers past that, recognized automatically rather than
// left as an opaque tagged byte string.
constexpr bool test_decode_tag_recognizes_positive_bignum() {
  constexpr std::uint8_t vec[] = {0xc2, 0x49, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& b = std::get<CborValue::BigNum>(d->first.data);
  return !b.negative && b.magnitude.size() == 9 && b.magnitude[0] == 1;
}
static_assert(test_decode_tag_recognizes_positive_bignum());

constexpr bool test_decode_tag_recognizes_negative_bignum() {
  constexpr std::uint8_t vec[] = {0xc3, 0x49, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& b = std::get<CborValue::BigNum>(d->first.data);
  return b.negative && b.magnitude.size() == 9;
}
static_assert(test_decode_tag_recognizes_negative_bignum());

constexpr bool test_decode_tag_leaves_ordinary_tags_alone() {
  // tag 1 (epoch time) must stay a generic Tag, not get swept into BigNum
  // just because 1 isn't 2 or 3.
  constexpr std::uint8_t vec[] = {0xc1, 0x1a, 0x51, 0x4b, 0x67, 0xb0};
  const auto d = decode_value(vec);
  return d && std::holds_alternative<CborValue::Tag>(d->first.data);
}
static_assert(test_decode_tag_leaves_ordinary_tags_alone());

// RFC 8949 Appendix A: 0("2013-03-21T20:04:00Z") -> a date-time string tag,
// 24(h'6449455446') -> tag 24 (encoded CBOR data item) wrapping a byte
// string, 32("http://www.example.com") -> a URI tag. All three are purely
// structural here (Tag + nested value): this library doesn't interpret
// tag semantics beyond the bignum tags, which is a deliberate scope limit,
// not an oversight -- a caller who needs date-time/URI/embedded-CBOR
// semantics builds that on top of the Tag value this already exposes.
constexpr bool test_decode_tag_matches_datetime_vector() {
  constexpr std::uint8_t vec[] = {0xc0, 0x74, 0x32, 0x30, 0x31, 0x33, 0x2d, 0x30, 0x33, 0x2d, 0x32,
                                   0x31, 0x54, 0x32, 0x30, 0x3a, 0x30, 0x34, 0x3a, 0x30, 0x30, 0x5a};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& tag = std::get<CborValue::Tag>(d->first.data);
  const auto& text = std::get<CborValue::OwnedString>(tag.value.front().data);
  return tag.number == 0 && text.kind == CborValue::StringKind::Text && text.content.size() == 20 &&
         d->second == sizeof(vec);
}
static_assert(test_decode_tag_matches_datetime_vector());

constexpr bool test_decode_tag_matches_embedded_cbor_vector() {
  constexpr std::uint8_t vec[] = {0xd8, 0x18, 0x45, 0x64, 0x49, 0x45, 0x54, 0x46};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& tag = std::get<CborValue::Tag>(d->first.data);
  const auto& bytes_value = std::get<CborValue::OwnedString>(tag.value.front().data);
  return tag.number == 24 && bytes_value.kind == CborValue::StringKind::Bytes && bytes_value.content.size() == 5;
}
static_assert(test_decode_tag_matches_embedded_cbor_vector());

constexpr bool test_decode_tag_matches_uri_vector() {
  constexpr std::uint8_t vec[] = {0xd8, 0x20, 0x76, 0x68, 0x74, 0x74, 0x70, 0x3a, 0x2f, 0x2f, 0x77,
                                   0x77, 0x77, 0x2e, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e,
                                   0x63, 0x6f, 0x6d};
  const auto d = decode_value(vec);
  if (!d) return false;
  const auto& tag = std::get<CborValue::Tag>(d->first.data);
  const auto& text = std::get<CborValue::OwnedString>(tag.value.front().data);
  return tag.number == 32 && text.content.size() == 22 && d->second == sizeof(vec);
}
static_assert(test_decode_tag_matches_uri_vector());

constexpr bool test_bignum_roundtrips() {
  constexpr std::uint8_t vec[] = {0xc2, 0x49, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const auto decoded = decode_value(vec);
  if (!decoded) return false;
  const auto reencoded = encode_value(decoded->first);
  return std::equal(reencoded.begin(), reencoded.end(), vec);
}
static_assert(test_bignum_roundtrips());

int main() { return 0; }
