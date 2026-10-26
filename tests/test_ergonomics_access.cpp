#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;
using namespace CompileTimeUnitTesting;

// operator[] exact-match retrieval, plus the miss-case sentinel's identity
// (the same CborValue::null_sentinel address every time, no per-call copy
// or allocation) -- proven both via static_assert (consteval, the whole
// point of this library) and a plain runtime check, since the rejected
// function-local-static sentinel design would have failed exactly the
// consteval path this file exercises.
//
// Each check below is a self-contained helper (build + query inside one
// function body), matching test_arena.cpp's established convention: a
// named variable from an *earlier statement* can't be referenced forming
// a later expect_eq<>()/expect_true<>() NTTP, but one whose lifetime began
// within that same evaluation (i.e. entirely inside its own helper) can.
constexpr std::uint64_t array_elem(std::size_t i) {
  return CborValue::array({10, 20, 30})[i].get<std::uint64_t>().value_or(0);
}
constexpr bool array_oob_is_undefined() { return CborValue::array({10, 20, 30})[99].is_undefined(); }
constexpr std::uint64_t map_lookup(const char* key) {
  return CborValue::object({{"a", 1}, {"b", 2}})[key].get<std::uint64_t>().value_or(0);
}
constexpr bool map_missing_is_undefined() { return CborValue::object({{"a", 1}})["missing"].is_undefined(); }

consteval bool test_array_and_map_access() {
  expect_eq<array_elem(0), std::uint64_t{10}>();
  expect_eq<array_elem(2), std::uint64_t{30}>();
  expect_true<array_oob_is_undefined()>();  // out-of-range read: sentinel, not UB/crash
  expect_eq<map_lookup("a"), std::uint64_t{1}>();
  expect_true<map_missing_is_undefined()>();
  return true;
}
static_assert(test_array_and_map_access());

constexpr bool test_sentinel_identity_const() {
  const CborValue arr = CborValue::array({1});
  const CborValue& miss_a = arr[5];
  const CborValue& miss_b = arr[6];
  // Both misses must bind to the exact same shared object -- no per-call
  // sentinel copy -- proven by address identity, not just value equality.
  return &miss_a == &miss_b && &miss_a == &null_sentinel;
}
static_assert(test_sentinel_identity_const());

bool test_sentinel_identity_runtime() {
  const CborValue arr = CborValue::array({1});
  const CborValue& miss_a = arr[5];
  const CborValue& miss_b = arr[6];
  return &miss_a == &miss_b && &miss_a == &null_sentinel;
}

constexpr bool test_array_of_arrays() {
  const CborValue grid = CborValue::array({CborValue::array({1, 2}), CborValue::array({3, 4})});
  if (grid[0][1].get<std::uint64_t>() != 2) return false;
  if (grid[1][0].get<std::uint64_t>() != 3) return false;
  if (!grid[9][9].is_undefined()) return false;  // chained miss stays undefined, no crash
  return true;
}
static_assert(test_array_of_arrays());

int main() {
  return (test_array_and_map_access() && test_sentinel_identity_const() && test_sentinel_identity_runtime() &&
          test_array_of_arrays())
             ? 0
             : 1;
}
