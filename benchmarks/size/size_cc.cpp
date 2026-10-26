#include "constexpr_cbor.hpp"
extern "C" bool decode_entry(const unsigned char* p, unsigned long n) {
  return ConstexprCbor::decode_value(std::span<const std::uint8_t>(p, n)).has_value();
}
