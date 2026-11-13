// A duplicate-key policy without the NoAlloc-shaped on_duplicate must fail to
// compile with a clear message (NoAlloc::is_duplicate_key_policy_v).
#include "constexpr_cbor_noalloc.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::NoAlloc;

struct WrongShapeKeyPolicy {
  static constexpr bool on_duplicate(int) { return true; }  // wrong signature entirely
};

constexpr std::uint8_t vec[] = {0xa0};  // {}

constexpr bool trigger() {
  NoAllocStorage<16, 16, 32> storage;
  auto pool = storage.make_pool();
  const auto result = decode_value<32, NoUtf8Validation, WrongShapeKeyPolicy>(pool, vec);
  return result.has_value();
}
constexpr bool bad_instantiation = trigger();

int main() { return 0; }
