// Runs NoAlloc::decode_value/encode_value as ordinary runtime code (every other
// NoAlloc test is consteval), including the NOALLOC_ASSERT(owns(...)) checks.
// Standalone, without compile_time_ut, like noexcept_tests.cpp (the bare-metal
// images copy only include/).
#include "constexpr_cbor_noalloc.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::NoAlloc;

bool run_noalloc_roundtrip(std::span<const std::uint8_t> bytes, std::span<std::uint8_t> out) {
  NoAllocStorage<16, 16, 32> storage;
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

bool run_noalloc_duplicate_key_and_nesting(std::span<const std::uint8_t> duplicate_key_map,
                                          std::span<const std::uint8_t> nested_indefinite_array) {
  NoAllocStorage<16, 16, 32> storage_a;
  auto pool_a = storage_a.make_pool();
  if (decode_value<32, NoUtf8Validation, RejectDuplicateKeys>(pool_a, duplicate_key_map).has_value()) {
    return false;
  }
  NoAllocStorage<16, 16, 32> storage_b;
  auto pool_b = storage_b.make_pool();
  const auto kept = decode_value<32, NoUtf8Validation, KeepLastDuplicateKey>(pool_b, duplicate_key_map);
  if (!kept) {
    return false;
  }
  NoAllocStorage<16, 16, 32> storage_c;
  auto pool_c = storage_c.make_pool();
  return decode_value(pool_c, nested_indefinite_array).has_value();
}

// NodePool::owns(): true for a pointer this pool produced, false otherwise. A
// triggered NOALLOC_ASSERT is not tested (compile error or abort()).
bool run_owns_check() {
  NoAllocStorage<8, 8, 32> storage;
  auto pool = storage.make_pool();
  constexpr std::uint8_t one_vec[] = {0x01};
  const auto decoded = decode_value(pool, one_vec);
  if (!decoded || !pool.owns(decoded->node)) {
    return false;
  }
  NoAllocStorage<8, 8, 32> other_storage;  // a second, unrelated pool
  auto other_pool = other_storage.make_pool();
  const auto other_decoded = decode_value(other_pool, one_vec);
  if (!other_decoded || pool.owns(other_decoded->node)) {
    return false;
  }
  return !pool.owns(nullptr);
}

int main() {
  constexpr std::uint8_t array_vec[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]
  std::uint8_t out[16] = {};
  if (!run_noalloc_roundtrip(array_vec, out)) {
    return 1;
  }
  constexpr std::uint8_t duplicate_key_map[] = {0xa2, 0x01, 0x02, 0x01, 0x03};  // {1:2, 1:3}
  constexpr std::uint8_t nested_indefinite_array[] = {0x9f, 0x01, 0x82, 0x02, 0x03, 0xff};  // [_ 1, [2,3]]
  if (!run_noalloc_duplicate_key_and_nesting(duplicate_key_map, nested_indefinite_array)) {
    return 1;
  }
  if (!run_owns_check()) {
    return 1;
  }
  return 0;
}
