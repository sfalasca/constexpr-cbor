// constexpr-cbor, heap mode (ConstexprCbor::CborValue)
// ---------------------------------------------------------------------
// The library's default, most ergonomic decode path: every decoded value
// is an owned CborValue (arrays/maps hold std::vector<CborValue>, the same
// shape as nlohmann::json's `json` object). This is the right mode to
// reach for first -- pick one of the other two example files only once
// you actually need to avoid the heap (NoAlloc) or avoid building a tree
// at all (SAX).
//
// Everything below also runs inside a consteval function (see the
// library's own test suite in tests/) -- it's written here as ordinary
// runtime code purely so this file can be compiled into a program you run
// and read the output of, not because the API is any different at
// compile time vs. runtime.
#include <iostream>

#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

namespace {

// A user type hooked into CborValue's get<T>()/converting-constructor
// machinery via plain, unqualified to_cbor/from_cbor functions found by
// ADL in the type's own namespace -- no CborValue-side registration
// needed. See README.md's "User types" section for the CborSerializer<T>
// alternative, for types you can't attach a free function to.
struct Point {
  int x, y;
};
constexpr bool to_cbor(CborValue& out, const Point& p) {
  out = CborValue::object({{"x", p.x}, {"y", p.y}});
  return true;
}
constexpr bool from_cbor(const CborValue& in, Point& p) {
  const auto x = in["x"].get<std::int64_t>();
  const auto y = in["y"].get<std::int64_t>();
  if (!x || !y) return false;
  p = {static_cast<int>(*x), static_cast<int>(*y)};
  return true;
}

void decode_and_inspect() {
  std::cout << "--- decode + ergonomic access ---\n";

  // RFC 8949 Appendix A: {"a": 1, "b": [2, 3]} -> 0xa26161016162820203
  constexpr std::uint8_t bytes[] = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03};

  // decode_value returns std::optional<std::pair<CborValue, bytes_consumed>>
  // -- std::nullopt on anything malformed, never a thrown exception or an
  // out-of-bounds read.
  const auto decoded = decode_value(bytes);
  if (!decoded) {
    std::cout << "decode failed\n";
    return;
  }
  const CborValue& value = decoded->first;

  // operator[] (by key or index), .get<T>() (returns std::optional<T>, no
  // throw on a miss or type mismatch), and range-for all work the same
  // way nlohmann::json's do.
  std::cout << "a = " << *value["a"].get<std::uint64_t>() << '\n';
  std::cout << "b[1] = " << *value["b"][1].get<std::uint64_t>() << '\n';
  std::cout << "missing key is_undefined() = " << value["nope"].is_undefined() << '\n';

  for (const auto& v : value["b"]) {
    std::cout << "b element: " << *v.get<std::uint64_t>() << '\n';
  }
}

void build_and_encode() {
  std::cout << "\n--- brace-init construction + canonical encode ---\n";

  // Array/map literal construction, nlohmann-style: a nested 2-element
  // array in the braced list is read as a key/value pair.
  CborValue config = {{"name", "sensor-1"}, {"thresholds", {10, 20, 30}}};
  config["enabled"] = true;  // auto-promotes Undefined -> part of the map

  // encode_value always emits the canonical (shortest) form -- useful
  // when you need a deterministic wire representation, e.g. for hashing
  // or signing a CBOR payload.
  const auto bytes = encode_value(config);
  std::cout << "encoded " << bytes.size() << " bytes\n";

  const auto roundtripped = decode_value(bytes);
  std::cout << "round-trips back to the same value: " << (roundtripped && roundtripped->first == config) << '\n';
}

void user_type_hook() {
  std::cout << "\n--- user-type ADL hook (to_cbor/from_cbor) ---\n";

  const CborValue encoded = Point{3, 4};  // implicit conversion via to_cbor
  const auto p = encoded.get<Point>();    // explicit conversion via from_cbor
  std::cout << "Point round-trip: (" << p->x << ", " << p->y << ")\n";
}

void policy_choices() {
  std::cout << "\n--- pluggable UTF-8 / duplicate-key policies ---\n";

  // RFC 8949 Section 5.3.1's own overlong-UTF-8 example: a single
  // continuation byte encoded via the 2-byte lead-byte form that should
  // have been the 1-byte form. Structurally well-formed CBOR, not valid
  // UTF-8 -- whether that matters is a caller choice, not a library
  // opinion, so it's a template parameter.
  constexpr std::uint8_t overlong_utf8[] = {0x62, 0xc0, 0xae};
  std::cout << "accepted under NoUtf8Validation (the default): "
            << decode_value(overlong_utf8).has_value() << '\n';
  std::cout << "accepted under CompleteUtf8Validation: "
            << decode_value<32, CompleteUtf8Validation>(overlong_utf8).has_value() << '\n';

  // {1: 2, 1: 3} -- a structurally well-formed map with a duplicate key.
  // RFC 8949 leaves this implementation-defined; constexpr-cbor makes it
  // a caller choice via the KeyPolicy template parameter instead of
  // hard-coding one answer.
  constexpr std::uint8_t duplicate_key_map[] = {0xa2, 0x01, 0x02, 0x01, 0x03};
  std::cout << "RejectDuplicateKeys (the default) refuses it: "
            << !decode_value(duplicate_key_map).has_value() << '\n';
  const auto kept_last = decode_value<32, NoUtf8Validation, KeepLastDuplicateKey>(duplicate_key_map);
  // The map's key is an integer, not a string -- operator[](const
  // CborValue&) looks up by any key type, not just operator[](string).
  const std::uint64_t surviving_value = *kept_last->first[CborValue(std::uint64_t{1})].get<std::uint64_t>();
  std::cout << "KeepLastDuplicateKey's surviving value for key 1: " << surviving_value << " (expected 3)\n";
}

}  // namespace

int main() {
  decode_and_inspect();
  build_and_encode();
  user_type_hook();
  policy_choices();
  return 0;
}
