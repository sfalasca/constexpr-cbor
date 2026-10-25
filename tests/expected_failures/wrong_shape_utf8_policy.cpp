// A UTF-8 validation policy missing the right static method shape must fail
// to compile with a clear, named message, not an obscure SFINAE dead end.
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

struct WrongShapePolicy {
  static constexpr bool is_valid(int) { return true; }  // wrong parameter type
};

constexpr std::uint8_t vec[] = {0x60};
constexpr auto bad_instantiation = decode_text_string<WrongShapePolicy>(vec);

int main() { return 0; }
