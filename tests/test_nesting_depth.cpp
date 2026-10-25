#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

// [[[1]]] -> 0x818181 01 is exactly 3 levels deep (the outermost array is
// depth 0; its one element, another array, is depth 1; its one element,
// another array, is depth 2; its one element, the integer 1, is depth 3's
// content but doesn't itself descend further).
consteval bool test_nesting_depth_accepts_exactly_at_the_bound() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec[] = {0x81, 0x81, 0x81, 0x01};
  expect_eq<decode_value<3>(vec).has_value(), true>();
  return true;
}
static_assert(test_nesting_depth_accepts_exactly_at_the_bound());

consteval bool test_nesting_depth_refuses_one_level_past_the_bound() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec[] = {0x81, 0x81, 0x81, 0x01};
  expect_eq<decode_value<2>(vec).has_value(), false>();
  return true;
}
static_assert(test_nesting_depth_refuses_one_level_past_the_bound());

// The default bound (32) must accept ordinary, shallow real-world structures
// without the caller ever having to think about it.
consteval bool test_nesting_depth_default_bound_is_generous_enough_for_ordinary_input() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec[] = {0x83, 0x01, 0x82, 0x02, 0x03, 0x82, 0x04, 0x05};  // [1, [2, 3], [4, 5]]
  expect_eq<decode_value(vec).has_value(), true>();
  return true;
}
static_assert(test_nesting_depth_default_bound_is_generous_enough_for_ordinary_input());

// Indefinite-length nesting is bounded the same way as definite-length.
consteval bool test_nesting_depth_bounds_indefinite_length_too() {
  using namespace CompileTimeUnitTesting;
  // [_ [_ [_ 1]]] -- three indefinite arrays nested three deep.
  constexpr std::uint8_t vec[] = {0x9f, 0x9f, 0x9f, 0x01, 0xff, 0xff, 0xff};
  expect_eq<decode_value<3>(vec).has_value(), true>();
  expect_eq<decode_value<2>(vec).has_value(), false>();
  return true;
}
static_assert(test_nesting_depth_bounds_indefinite_length_too());

int main() { return 0; }
