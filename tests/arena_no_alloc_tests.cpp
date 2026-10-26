// Proves Arena::decode_value/encode_value genuinely work at *runtime*, not
// just inside a consteval function: every other Arena test only exercises
// them inside consteval functions/static_assert, where the compiler's own
// constant-evaluation machinery handles any transient allocation
// internally -- a compile-time-only proof says nothing about what actually
// runs on a target decoding a real sensor or network buffer at runtime.
// The functions below are ordinary (non-constexpr) functions instead, so
// their bodies are real, compiled, executed code; ctest runs the compiled
// binary and checks its exit code like any other test in this suite. This
// is also the one file where the ARENA_ASSERT(owns(...)) checks wired
// into NodePool's accessors run as genuine, compiled runtime checks
// rather than participating in constant evaluation.
//
// Deleting the global operator new/delete to additionally prove no
// allocation happens at compile-enforced link time was tried and
// abandoned: a deleted definition must be the function's first
// declaration, which it can't be once <new> (pulled in transitively by
// <vector>/<array>/etc.) has already declared it undeleted -- there's no
// portable way to turn "this calls operator new" into a hard compile
// error after the fact. Standalone: no compile_time_ut dependency,
// matching noexcept_tests.cpp's own convention (the bare-metal images
// only copy include/, not a fetched sibling library).
#include "constexpr_cbor_arena.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Arena;

// Deliberately a plain function, not constexpr/consteval: its body is
// ordinary runtime code the compiler must actually generate, the same as
// it would for a real caller decoding a byte buffer that only exists at
// runtime (sensor data, a network packet, ...).
bool run_arena_roundtrip(std::span<const std::uint8_t> bytes, std::span<std::uint8_t> out) {
  ArenaStorage<16, 16, 32> storage;
  auto pool = storage.make_pool();
  const auto decoded = decode_value(pool, bytes);
  if (!decoded) {
    return false;
  }
  const auto written = encode_value(pool, decoded->node, out);
  if (!written || *written != bytes.size()) {
    return false;
  }
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (out[i] != bytes[i]) {
      return false;
    }
  }
  return true;
}

// Also exercises the duplicate-key policy and a nested/indefinite-length
// shape at runtime, not just the flat-array happy path above.
bool run_arena_duplicate_key_and_nesting(std::span<const std::uint8_t> duplicate_key_map,
                                          std::span<const std::uint8_t> nested_indefinite_array) {
  ArenaStorage<16, 16, 32> storage_a;
  auto pool_a = storage_a.make_pool();
  if (decode_value<32, NoUtf8Validation, RejectDuplicateKeys>(pool_a, duplicate_key_map).has_value()) {
    return false;  // must be refused
  }
  ArenaStorage<16, 16, 32> storage_b;
  auto pool_b = storage_b.make_pool();
  const auto kept = decode_value<32, NoUtf8Validation, KeepLastDuplicateKey>(pool_b, duplicate_key_map);
  if (!kept) {
    return false;
  }
  ArenaStorage<16, 16, 32> storage_c;
  auto pool_c = storage_c.make_pool();
  return decode_value(pool_c, nested_indefinite_array).has_value();
}

// Plain-predicate check of NodePool::owns() itself (see its own comment
// for why a *triggered* ARENA_ASSERT at one of the call sites that uses it
// isn't independently crash-tested here: it's either a hard compile error
// during consteval or an abort() at runtime, neither of which is something
// this test suite's normal pass/fail convention can observe without a
// death-test harness this project doesn't otherwise use). This proves the
// check itself is correct: true for a pointer this pool actually produced,
// false for one that didn't come from it at all.
bool run_owns_check() {
  ArenaStorage<8, 8, 32> storage;
  auto pool = storage.make_pool();
  constexpr std::uint8_t one_vec[] = {0x01};
  const auto decoded = decode_value(pool, one_vec);
  if (!decoded || !pool.owns(decoded->node)) {
    return false;  // a genuinely-owned pointer must report true
  }
  ArenaStorage<8, 8, 32> other_storage;  // a second, unrelated pool
  auto other_pool = other_storage.make_pool();
  const auto other_decoded = decode_value(other_pool, one_vec);
  if (!other_decoded || pool.owns(other_decoded->node)) {
    return false;  // a pointer from a different pool must report false
  }
  return !pool.owns(nullptr);
}

int main() {
  constexpr std::uint8_t array_vec[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]
  std::uint8_t out[16] = {};
  if (!run_arena_roundtrip(array_vec, out)) {
    return 1;
  }
  constexpr std::uint8_t duplicate_key_map[] = {0xa2, 0x01, 0x02, 0x01, 0x03};  // {1:2, 1:3}
  constexpr std::uint8_t nested_indefinite_array[] = {0x9f, 0x01, 0x82, 0x02, 0x03, 0xff};  // [_ 1, [2,3]]
  if (!run_arena_duplicate_key_and_nesting(duplicate_key_map, nested_indefinite_array)) {
    return 1;
  }
  if (!run_owns_check()) {
    return 1;
  }
  return 0;
}
