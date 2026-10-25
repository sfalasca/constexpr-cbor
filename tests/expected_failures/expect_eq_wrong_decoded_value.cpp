// A straightforward wrong expectation against the real decoder, proving the
// test harness itself actually fails loudly rather than silently passing.
#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

consteval bool test_deliberately_wrong_expectation() {
  using namespace CompileTimeUnitTesting;
  constexpr std::uint8_t vec_23[] = {0x17};  // the real value is 23
  expect_eq<decode_uint(vec_23)->value, std::uint64_t{24}>();  // deliberately wrong
  return true;
}
static_assert(test_deliberately_wrong_expectation());

int main() { return 0; }
