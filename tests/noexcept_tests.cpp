// Built with -fno-exceptions -fno-rtti (docker esp32, stm32 and riscv32 images) to
// prove the decoder and encoder surface needs neither. Standalone: those images
// copy only include/.
#include "constexpr_cbor.hpp"
#include "constexpr_cbor_sax.hpp"

using namespace ConstexprCbor;

constexpr std::uint8_t array_vec[] = {0x83, 0x01, 0x02, 0x03};  // [1, 2, 3]

constexpr bool test_decode_and_encode_roundtrip() {
  const auto decoded = decode_value(array_vec);
  if (!decoded) {
    return false;
  }
  const auto reencoded = encode_value(decoded->first);
  bool matches = reencoded.size() == sizeof(array_vec);
  for (std::size_t i = 0; matches && i < reencoded.size(); ++i) {
    matches = reencoded[i] == array_vec[i];
  }
  return matches;
}
static_assert(test_decode_and_encode_roundtrip());

constexpr bool test_uint_and_negint() {
  constexpr std::uint8_t uint_vec[] = {0x17};
  constexpr std::uint8_t negint_vec[] = {0x20};
  return decode_uint(uint_vec)->value == 23 && decode_negint(negint_vec)->value.to_int64() == -1;
}
static_assert(test_uint_and_negint());

constexpr bool test_sax_decode() {
  std::size_t items = 0;
  for (auto ev : Sax::SaxRange<32>{array_vec}) {
    if (ev.kind == Sax::EventKind::Error) return false;
    ++items;
  }
  return items == 5;  // BeginArray, 1, 2, 3, EndArray
}
static_assert(test_sax_decode());

int main() { return 0; }
