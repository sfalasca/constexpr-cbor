#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

consteval bool test_decode_length_accepts_direct_values() {
  using namespace CompileTimeUnitTesting;
  constexpr DecodedUint sentinel{.value = 999, .bytes_consumed = 999};
  constexpr std::uint8_t zero[] = {0x00};
  constexpr std::uint8_t mid[] = {0x0b};
  constexpr std::uint8_t max[] = {0x17};

  expect_eq<decode_length(zero).has_value(), true>();
  expect_eq<decode_length(zero).value_or(sentinel).value, std::uint64_t{0}>();
  expect_eq<decode_length(mid).value_or(sentinel).value, std::uint64_t{11}>();
  expect_eq<decode_length(max).value_or(sentinel).value, std::uint64_t{23}>();
  return true;
}
static_assert(test_decode_length_accepts_direct_values());

// Official RFC 8949 Appendix A vectors for the 1/2/4/8-byte extended lengths.
consteval bool test_decode_length_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr DecodedUint sentinel{.value = 999, .bytes_consumed = 999};
  constexpr std::uint8_t vec_24[] = {0x18, 0x18};
  constexpr std::uint8_t vec_1000[] = {0x19, 0x03, 0xe8};
  constexpr std::uint8_t vec_1000000[] = {0x1a, 0x00, 0x0f, 0x42, 0x40};
  constexpr std::uint8_t vec_max_u64[] = {0x1b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

  expect_eq<decode_length(vec_24).value_or(sentinel).value, std::uint64_t{24}>();
  expect_eq<decode_length(vec_24).value_or(sentinel).bytes_consumed, std::size_t{2}>();
  expect_eq<decode_length(vec_1000).value_or(sentinel).value, std::uint64_t{1000}>();
  expect_eq<decode_length(vec_1000000).value_or(sentinel).value, std::uint64_t{1000000}>();
  expect_eq<decode_length(vec_max_u64).value_or(sentinel).value, std::numeric_limits<std::uint64_t>::max()>();
  return true;
}
static_assert(test_decode_length_matches_official_vectors());

consteval bool test_decode_length_rejects_unassigned_markers() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t lowest_unhandled[] = {0x1c};   // additional info 28: reserved
  constexpr std::uint8_t highest_unhandled[] = {0x1f};  // additional info 31: indefinite-length marker
  expect_eq<decode_length(lowest_unhandled).has_value(), false>();
  expect_eq<decode_length(highest_unhandled).has_value(), false>();
  return true;
}
static_assert(test_decode_length_rejects_unassigned_markers());

consteval bool test_decode_length_refuses_truncated_input() {
  using namespace CompileTimeUnitTesting;
  constexpr std::span<const std::uint8_t> empty{};
  constexpr std::uint8_t header_only[] = {0x18};
  expect_eq<decode_length(empty).has_value(), false>();
  expect_eq<decode_length(header_only).has_value(), false>();
  return true;
}
static_assert(test_decode_length_refuses_truncated_input());

int main() { return 0; }
