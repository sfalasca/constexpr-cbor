#include "constexpr_cbor_arena.hpp"
static ConstexprCbor::Arena::NodePool<32, 256> pool;
extern "C" bool decode_entry(const unsigned char* p, unsigned long n) {
  pool.node_count = 0; pool.byte_count = 0;
  return ConstexprCbor::Arena::decode_value(pool, std::span<const std::uint8_t>(p, n)).has_value();
}
