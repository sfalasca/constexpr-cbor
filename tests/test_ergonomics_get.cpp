#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;
using namespace CompileTimeUnitTesting;

consteval bool test_get_built_in_types() {
  expect_eq<CborValue(true).get<bool>().value(), true>();
  expect_true<!CborValue(std::uint64_t{1}).get<bool>().has_value()>();

  expect_eq<CborValue(std::uint64_t{5}).get<std::uint64_t>().value(), std::uint64_t{5}>();
  expect_true<!CborValue(-1).get<std::uint64_t>().has_value()>();

  expect_eq<CborValue(std::uint64_t{5}).get<std::int64_t>().value(), std::int64_t{5}>();
  expect_eq<CborValue(-5).get<std::int64_t>().value(), std::int64_t{-5}>();

  expect_eq<CborValue(1.5).get<double>().value(), 1.5>();
  expect_true<!CborValue(std::uint64_t{1}).get<double>().has_value()>();  // exact-type only, no int promotion

  expect_true<!CborValue(nullptr).get<std::monostate>().has_value()>();
  expect_true<CborValue().get<std::monostate>().has_value()>();
  expect_true<CborValue(nullptr).get<std::nullptr_t>().has_value()>();
  return true;
}
static_assert(test_get_built_in_types());

constexpr bool test_get_string_and_bytes() {
  if (CborValue("hello").get<std::string>() != "hello") return false;
  if (CborValue("hello").get<std::vector<std::uint8_t>>().has_value()) return false;  // wrong string kind
  const auto bin = CborValue::binary(std::array<std::uint8_t, 3>{1, 2, 3}).get<std::vector<std::uint8_t>>();
  if (!bin || *bin != std::vector<std::uint8_t>{1, 2, 3}) return false;
  return true;
}
static_assert(test_get_string_and_bytes());

// The NegativeInt-too-large-for-int64_t boundary: n == 2^64-1 means true
// value -2^64, which doesn't fit std::int64_t (min -2^63) -- must refuse,
// not silently truncate/wrap.
constexpr bool test_get_int64_overflow_boundary() {
  const CborValue huge_negative(NegativeInt{.n = std::numeric_limits<std::uint64_t>::max()});
  if (huge_negative.get<std::int64_t>().has_value()) return false;
  const CborValue max_negative(NegativeInt{.n = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())});
  if (!max_negative.get<std::int64_t>().has_value()) return false;  // exactly at the boundary: must succeed
  return true;
}
static_assert(test_get_int64_overflow_boundary());

int main() {
  return (test_get_built_in_types() && test_get_string_and_bytes() && test_get_int64_overflow_boundary()) ? 0 : 1;
}
