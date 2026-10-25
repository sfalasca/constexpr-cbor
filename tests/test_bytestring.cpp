#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: h'' -> 0x40, h'01020304' -> 0x4401020304.
consteval bool test_decode_byte_string_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_empty[] = {0x40};
  constexpr std::uint8_t vec_4bytes[] = {0x44, 0x01, 0x02, 0x03, 0x04};

  expect_eq<decode_byte_string(vec_empty)->bytes.size(), std::size_t{0}>();
  expect_eq<decode_byte_string(vec_4bytes)->bytes.size(), std::size_t{4}>();
  expect_eq<decode_byte_string(vec_4bytes)->bytes_consumed, std::size_t{5}>();
  return true;
}
static_assert(test_decode_byte_string_matches_official_vectors());

consteval bool test_decode_byte_string_rejects_wrong_major_type() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t wrong[] = {0x60};  // major type 3 (text string)
  expect_eq<decode_byte_string(wrong).has_value(), false>();
  return true;
}
static_assert(test_decode_byte_string_rejects_wrong_major_type());

consteval bool test_decode_byte_string_refuses_truncated_content() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t truncated[] = {0x44, 0x01, 0x02};  // says 4 bytes, gives 2
  expect_eq<decode_byte_string(truncated).has_value(), false>();
  return true;
}
static_assert(test_decode_byte_string_refuses_truncated_content());

consteval bool test_encode_byte_string_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<std::uint8_t, 5> content{0x01, 0x02, 0x03, 0x04, 0x00};
  constexpr std::array<std::uint8_t, 5> expected{0x44, 0x01, 0x02, 0x03, 0x04};
  const auto encoded = encode_byte_string(std::span(content).subspan(0, 4));
  expect_eq(std::span<const std::uint8_t>(encoded), std::span<const std::uint8_t>(expected));
  return true;
}
static_assert(test_encode_byte_string_matches_official_vectors());

consteval bool test_bytestring_roundtrips() {
  constexpr std::uint8_t vec[] = {0x44, 0x01, 0x02, 0x03, 0x04};
  const auto decoded = decode_byte_string(vec);
  if (!decoded) return false;
  const auto reencoded = encode_byte_string(decoded->bytes);
  return std::equal(reencoded.begin(), reencoded.end(), vec);
}
static_assert(test_bytestring_roundtrips());

int main() { return 0; }
