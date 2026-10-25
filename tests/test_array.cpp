#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// RFC 8949 Appendix A: [] -> 0x80, [1,2,3] -> 0x83010203,
// [1, [2, 3], [4, 5]] -> 0x8301820203820405.
//
// decode_value's result holds a std::vector, so each assertion below
// re-decodes fresh inline rather than caching into a named variable: a
// vector's heap allocation can be used transiently within one constant
// expression, but can't persist as a frozen constexpr object reused across
// several independent expect_eq<...>() template-argument formations.
consteval bool test_decode_array_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_empty[] = {0x80};
  constexpr std::uint8_t vec_123[] = {0x83, 0x01, 0x02, 0x03};

  expect_eq<decode_value(vec_empty).has_value(), true>();
  expect_eq<std::get<std::vector<CborValue>>(decode_value(vec_empty)->first.data).empty(), true>();
  expect_eq<std::get<std::vector<CborValue>>(decode_value(vec_123)->first.data).size(), std::size_t{3}>();
  expect_eq<std::get<std::uint64_t>(
                std::get<std::vector<CborValue>>(decode_value(vec_123)->first.data)[0].data),
            std::uint64_t{1}>();
  expect_eq<decode_value(vec_123)->second, std::size_t{4}>();
  return true;
}
static_assert(test_decode_array_matches_official_vectors());

// Here the decode result is only used as an ordinary local variable, inside
// plain function-body logic, not fed into an NTTP -- so a named variable is
// fine, the escaping-allocation rule only applies to the expect_eq<A, B>()
// template-argument form above.
constexpr bool test_decode_array_handles_nesting() {
  constexpr std::uint8_t vec_nested[] = {0x83, 0x01, 0x82, 0x02, 0x03, 0x82, 0x04, 0x05};
  const auto d = decode_value(vec_nested);
  if (!d) return false;
  const auto& outer = std::get<std::vector<CborValue>>(d->first.data);
  if (outer.size() != 3) return false;
  const auto& inner = std::get<std::vector<CborValue>>(outer[1].data);
  return inner.size() == 2 && std::get<std::uint64_t>(inner[0].data) == 2;
}
static_assert(test_decode_array_handles_nesting());

// RFC 8949 Appendix A: the 25-element array exercises the one-byte-length
// array-count marker (additional info 24).
constexpr bool test_decode_array_matches_25_element_vector() {
  constexpr std::uint8_t vec_25[] = {0x98, 0x19, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
                                      0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x18,
                                      0x18, 0x19};
  const auto d = decode_value(vec_25);
  if (!d) return false;
  const auto& items = std::get<std::vector<CborValue>>(d->first.data);
  return items.size() == 25 && std::get<std::uint64_t>(items[0].data) == 1 &&
         std::get<std::uint64_t>(items[24].data) == 25;
}
static_assert(test_decode_array_matches_25_element_vector());

consteval bool test_decode_array_refuses_truncated_input() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t truncated[] = {0x83, 0x01, 0x02};  // declares 3 items, only 2 present
  expect_eq<decode_value(truncated).has_value(), false>();
  return true;
}
static_assert(test_decode_array_refuses_truncated_input());

consteval bool test_encode_array_matches_official_vectors() {
  using namespace CompileTimeUnitTesting;
  constexpr std::array<std::uint8_t, 4> expected{0x83, 0x01, 0x02, 0x03};
  const CborValue value{.data = std::vector<CborValue>{
                             CborValue{.data = std::uint64_t{1}},
                             CborValue{.data = std::uint64_t{2}},
                             CborValue{.data = std::uint64_t{3}},
                         }};
  const auto encoded = encode_value(value);
  expect_eq(std::span<const std::uint8_t>(encoded), std::span<const std::uint8_t>(expected));
  return true;
}
static_assert(test_encode_array_matches_official_vectors());

constexpr bool test_array_roundtrips_nested() {
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x82, 0x02, 0x03, 0x82, 0x04, 0x05};
  const auto decoded = decode_value(vec);
  if (!decoded) return false;
  const auto reencoded = encode_value(decoded->first);
  return std::equal(reencoded.begin(), reencoded.end(), vec);
}
static_assert(test_array_roundtrips_nested());

int main() { return 0; }
