// A duplicate-key policy missing the right static method shape must fail to
// compile with a clear, named message.
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

struct WrongShapeKeyPolicy {
  static constexpr bool on_duplicate(int) { return true; }  // wrong signature entirely
};

constexpr std::uint8_t vec[] = {0xa0};  // {}
constexpr auto bad_instantiation = decode_value<32, NoUtf8Validation, WrongShapeKeyPolicy>(vec);

int main() { return 0; }
