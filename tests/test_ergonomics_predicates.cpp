#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;
using namespace CompileTimeUnitTesting;

// is_*() across every CborValue alternative, plus size()/empty()/contains(),
// matching nlohmann's own null=0/scalar=1/container=count convention.
consteval bool test_predicates_cover_every_alternative() {
  expect_true<CborValue().is_undefined()>();
  expect_true<CborValue(nullptr).is_null()>();
  expect_true<CborValue(true).is_boolean()>();
  expect_true<CborValue(std::uint64_t{5}).is_unsigned()>();
  expect_true<CborValue(-5).is_negative_int()>();
  expect_true<CborValue(CborValue::BigNum{.negative = false, .magnitude = {1, 2}}).is_bignum()>();
  expect_true<CborValue(1.5).is_float()>();
  expect_true<CborValue("hi").is_string()>();
  expect_true<CborValue::binary(std::array<std::uint8_t, 2>{1, 2}).is_binary()>();
  expect_true<CborValue(CborValue::SimpleValue{.number = 32}).is_simple()>();
  expect_true<CborValue::array({1, 2}).is_array()>();
  expect_true<CborValue::object({{"a", 1}}).is_map()>();
  expect_true<CborValue::object({{"a", 1}}).is_object()>();
  expect_true<CborValue(CborValue::Tag{.number = 0, .value = {CborValue(1)}}).is_tag()>();

  expect_true<!CborValue(std::uint64_t{5}).is_array()>();
  expect_true<!CborValue::array({1}).is_map()>();
  return true;
}
static_assert(test_predicates_cover_every_alternative());

constexpr bool test_is_integer_and_is_number() {
  if (!CborValue(std::uint64_t{1}).is_integer()) return false;
  if (!CborValue(-1).is_integer()) return false;
  if (!CborValue(CborValue::BigNum{.negative = false, .magnitude = {1}}).is_integer()) return false;
  if (CborValue(1.5).is_integer()) return false;
  if (!CborValue(1.5).is_number()) return false;
  if (!CborValue(std::uint64_t{1}).is_number()) return false;
  if (CborValue("x").is_number()) return false;
  return true;
}
static_assert(test_is_integer_and_is_number());

consteval bool test_size_matches_nlohmann_convention() {
  expect_eq<CborValue().size(), std::size_t{0}>();                 // undefined
  expect_eq<CborValue(nullptr).size(), std::size_t{0}>();          // null
  expect_eq<CborValue(std::uint64_t{5}).size(), std::size_t{1}>();  // scalar
  expect_eq<CborValue::array({1, 2, 3}).size(), std::size_t{3}>();
  expect_eq<CborValue::object({{"a", 1}, {"b", 2}}).size(), std::size_t{2}>();
  expect_true<CborValue().empty()>();
  expect_true<!CborValue::array({1}).empty()>();
  expect_true<CborValue::array({}).empty()>();
  return true;
}
static_assert(test_size_matches_nlohmann_convention());

constexpr bool test_contains_arbitrary_key_types() {
  const CborValue m = CborValue::object({{"a", 1}, {CborValue::array({1, 2}), "composite-key-value"}});
  if (!m.contains("a")) return false;
  if (m.contains("z")) return false;
  if (!m.contains(CborValue::array({1, 2}))) return false;  // a non-string (array) map key
  if (m.contains(CborValue::array({9, 9}))) return false;
  if (CborValue(std::uint64_t{5}).contains("a")) return false;  // scalar: never contains anything
  return true;
}
static_assert(test_contains_arbitrary_key_types());

int main() {
  return (test_predicates_cover_every_alternative() && test_is_integer_and_is_number() &&
          test_size_matches_nlohmann_convention() && test_contains_arbitrary_key_types())
             ? 0
             : 1;
}
