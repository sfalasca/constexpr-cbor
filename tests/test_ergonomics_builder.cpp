#include <algorithm>

#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;
using namespace CompileTimeUnitTesting;

// Brace-init round-trips through encode_value/decode_value to RFC 8949's
// own canonical bytes, plus the array()/object() escape hatches that
// bypass the array-vs-map heuristic.
constexpr bool array_literal_matches_canonical_bytes() {
  constexpr std::uint8_t expected[] = {0x83, 0x01, 0x02, 0x03};  // [1,2,3]
  const CborValue v = {1, 2, 3};
  const auto encoded = encode_value(v);
  return std::equal(encoded.begin(), encoded.end(), expected, expected + 4);
}

constexpr bool map_literal_matches_canonical_bytes() {
  // {1:2, 3:4} -> 0xa2 01 02 03 04 (RFC 8949 Appendix A).
  constexpr std::uint8_t expected[] = {0xa2, 0x01, 0x02, 0x03, 0x04};
  const CborValue v = {{1, 2}, {3, 4}};
  const auto encoded = encode_value(v);
  return std::equal(encoded.begin(), encoded.end(), expected, expected + 5);
}

constexpr bool object_with_non_string_key_round_trips() {
  const CborValue v = CborValue::object({{CborValue::array({1, 2}), "composite"}});
  const auto encoded = encode_value(v);
  const auto decoded = decode_value(encoded);
  if (!decoded) return false;
  return decoded->first[CborValue::array({1, 2})].get<std::string>() == "composite";
}

// CborValue::array({...}) must bypass the heuristic for an
// array-of-2-element-sub-arrays case the plain heuristic would misread as
// a map -- the single most important disambiguation property here.
constexpr bool forced_array_of_pairs_stays_an_array() {
  const CborValue plain_heuristic = {{1, 2}, {3, 4}};  // heuristic sees 2 pairs -> Map
  const CborValue forced = CborValue::array({CborValue::array({1, 2}), CborValue::array({3, 4})});
  return plain_heuristic.is_map() && forced.is_array() && forced.size() == 2 && forced[0][0].get<std::uint64_t>() == 1;
}

constexpr bool empty_array_and_object_distinguishable() {
  const CborValue empty_arr = CborValue::array();
  const CborValue empty_obj = CborValue::object({});
  return empty_arr.is_array() && empty_arr.empty() && empty_obj.is_map() && empty_obj.empty();
}

// Mutating assignment: auto-promote Undefined -> Map/Array on first []
// assignment, null-padding on array growth, overwrite-in-place (keep-last)
// on repeated key assignment -- independent of whatever KeyPolicy a given
// decode_value<...> call used (the builder isn't policy-templated).
constexpr bool assignment_auto_promotes_and_grows() {
  CborValue built;
  built["x"] = std::uint64_t{42};
  built["y"]["z"] = "hi";
  if (!built.is_map()) return false;
  if (built["x"].get<std::uint64_t>() != 42) return false;
  if (built["y"]["z"].get<std::string>() != "hi") return false;

  CborValue arr;
  arr[2] = std::uint64_t{9};
  if (!arr.is_array() || arr.size() != 3) return false;
  if (!arr[0].is_null() && !arr[0].is_undefined()) return false;  // null-padded, not garbage
  if (arr[2].get<std::uint64_t>() != 9) return false;
  return true;
}

constexpr bool repeated_key_assignment_overwrites_in_place() {
  CborValue m;
  m["k"] = std::uint64_t{1};
  m["k"] = std::uint64_t{2};
  return m.is_map() && m.size() == 1 && m["k"].get<std::uint64_t>() == 2;
}

consteval bool test_builder() {
  expect_true<array_literal_matches_canonical_bytes()>();
  expect_true<map_literal_matches_canonical_bytes()>();
  expect_true<object_with_non_string_key_round_trips()>();
  expect_true<forced_array_of_pairs_stays_an_array()>();
  expect_true<empty_array_and_object_distinguishable()>();
  expect_true<assignment_auto_promotes_and_grows()>();
  expect_true<repeated_key_assignment_overwrites_in_place()>();
  return true;
}
static_assert(test_builder());

int main() { return test_builder() ? 0 : 1; }
