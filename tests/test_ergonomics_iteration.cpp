#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;
using namespace CompileTimeUnitTesting;

constexpr std::uint64_t sum_array() {
  std::uint64_t total = 0;
  for (const auto& v : CborValue::array({1, 2, 3})) {
    total += v.get<std::uint64_t>().value_or(0);
  }
  return total;
}

// Iterating a map yields values only, matching nlohmann's default `for`.
constexpr std::uint64_t sum_map_values() {
  std::uint64_t total = 0;
  for (const auto& v : CborValue::object({{"a", 1}, {"b", 2}, {"c", 3}})) {
    total += v.get<std::uint64_t>().value_or(0);
  }
  return total;
}

constexpr std::size_t count_items() {
  std::size_t count = 0;
  for (auto m = CborValue::object({{"a", 1}, {"b", 2}}); const auto item : m.items()) {
    (void)item;
    ++count;
  }
  return count;
}

constexpr bool items_yields_key_and_value() {
  auto m = CborValue::object({{"a", 1}, {"b", 2}});
  bool saw_a = false, saw_b = false;
  for (const auto item : m.items()) {
    if (item.key.get<std::string>() == "a" && item.value.get<std::uint64_t>() == 1) saw_a = true;
    if (item.key.get<std::string>() == "b" && item.value.get<std::uint64_t>() == 2) saw_b = true;
  }
  return saw_a && saw_b;
}

constexpr bool items_yields_non_string_key() {
  auto m = CborValue::object({{CborValue::array({1, 2}), "composite"}});
  for (const auto item : m.items()) {
    if (item.key.is_array() && item.value.get<std::string>() == "composite") return true;
  }
  return false;
}

// Iterating a scalar: not supported, range-for yields an empty range
// rather than failing to compile or crashing.
constexpr std::size_t count_scalar_iteration() {
  std::size_t count = 0;
  for (const auto& v : CborValue(std::uint64_t{42})) {
    (void)v;
    ++count;
  }
  return count;
}

// .items() on a non-map: empty range, not ill-formed.
constexpr std::size_t count_items_on_non_map() {
  std::size_t count = 0;
  for (auto arr = CborValue::array({1, 2}); const auto item : arr.items()) {
    (void)item;
    ++count;
  }
  return count;
}

consteval bool test_iteration() {
  expect_eq<sum_array(), std::uint64_t{6}>();
  expect_eq<sum_map_values(), std::uint64_t{6}>();
  expect_eq<count_items(), std::size_t{2}>();
  expect_true<items_yields_key_and_value()>();
  expect_true<items_yields_non_string_key()>();
  expect_eq<count_scalar_iteration(), std::size_t{0}>();
  expect_eq<count_items_on_non_map(), std::size_t{0}>();
  return true;
}
static_assert(test_iteration());

int main() { return test_iteration() ? 0 : 1; }
