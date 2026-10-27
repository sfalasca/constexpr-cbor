// See size_cc.cpp's comment -- same fairness fix, NoAlloc mode.
#include "constexpr_cbor_noalloc.hpp"

namespace {
volatile std::uint64_t sink = 0;
ConstexprCbor::NoAlloc::NoAllocStorage<64, 64, 512> storage;
ConstexprCbor::NoAlloc::NodePool pool = storage.make_pool();

void walk(const ConstexprCbor::NoAlloc::Node& node) {
  using namespace ConstexprCbor::NoAlloc;
  sink = sink + 1;
  std::visit(
      [](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, ArrayRef>) {
          for (std::size_t i = 0; i < x.count; ++i) walk(pool.child(x, i));
        } else if constexpr (std::is_same_v<T, MapRef>) {
          for (std::size_t i = 0; i < x.count; ++i) {
            walk(pool.map_key(x, i));
            walk(pool.map_value(x, i));
          }
        } else if constexpr (std::is_same_v<T, TagRef>) {
          walk(pool.tag_child(x));
        } else if constexpr (std::is_same_v<T, StringRef>) {
          sink += pool.bytes_of(x).size();
        }
      },
      node.data);
}
}  // namespace

extern "C" bool decode_entry(const unsigned char* p, unsigned long n) {
  pool.node_count_ = 0;
  pool.ref_count_ = 0;
  pool.byte_count_ = 0;
  const auto decoded = ConstexprCbor::NoAlloc::decode_value(pool, std::span<const std::uint8_t>(p, n));
  if (!decoded) return false;
  walk(*decoded->node);
  return true;
}
