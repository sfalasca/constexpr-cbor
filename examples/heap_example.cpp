// constexpr-cbor heap mode: every decoded value is an owned CborValue, with an
// nlohmann::json-style interface. Also works in consteval; written as runtime
// code so the output can be printed.
#include <iostream>

#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;

namespace {

// User type hooked in through unqualified to_cbor/from_cbor found by ADL. For
// types that cannot take a free function, specialize CborSerializer<T> (README).
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

// User type hooked in through cbor_fields<T>(): listing the fields is enough for
// the default CborSerializer<T>. Use the ADL hooks instead for custom logic or a
// CBOR shape that does not mirror the struct.
struct Vec3 {
  double x, y, z;
};

}  // namespace

namespace ConstexprCbor {
template <>
constexpr auto cbor_fields<Vec3>() {
  return std::tuple{std::pair{"x", &Vec3::x}, std::pair{"y", &Vec3::y}, std::pair{"z", &Vec3::z}};
}
}  // namespace ConstexprCbor

namespace {

void decode_and_inspect() {
  std::cout << "--- decode + ergonomic access ---\n";

  // {"a": 1, "b": [2, 3]}
  constexpr std::uint8_t bytes[] = {0xa2, 0x61, 0x61, 0x01, 0x61, 0x62, 0x82, 0x02, 0x03};

  // decode_value returns Expected<std::pair<CborValue, bytes_consumed>, Error>; it never throws.
  const auto decoded = decode_value(bytes);
  if (!decoded) {
    std::cout << "decode failed\n";
    return;
  }
  const CborValue& value = decoded->first;

  std::cout << "a = " << *value["a"].get<std::uint64_t>() << '\n';
  std::cout << "b[1] = " << *value["b"][1].get<std::uint64_t>() << '\n';
  std::cout << "missing key is_undefined() = " << value["nope"].is_undefined() << '\n';

  for (const auto& v : value["b"]) {
    std::cout << "b element: " << *v.get<std::uint64_t>() << '\n';
  }
}

void build_and_encode() {
  std::cout << "\n--- brace-init construction + canonical encode ---\n";

  // A nested 2-element array in a braced list is read as a key/value pair.
  CborValue config = {{"name", "sensor-1"}, {"thresholds", {10, 20, 30}}};
  config["enabled"] = true;  // auto-promotes Undefined -> part of the map

  // encode_value always emits the shortest form, so output is deterministic (hashing, signing).
  const auto bytes = encode_value(config);
  std::cout << "encoded " << bytes.size() << " bytes\n";

  const auto roundtripped = decode_value(bytes);
  std::cout << "round-trips back to the same value: " << (roundtripped && roundtripped->first == config) << '\n';
}

void user_type_hook() {
  std::cout << "\n--- user-type ADL hook (to_cbor/from_cbor) ---\n";

  const CborValue encoded = Point{3, 4};
  const auto p = encoded.get<Point>();
  std::cout << "Point round-trip: (" << p->x << ", " << p->y << ")\n";
}

void user_type_cbor_fields() {
  std::cout << "\n--- user-type reflection hook (cbor_fields<T>()) ---\n";

  const CborValue encoded = Vec3{1.0, 2.0, 3.0};
  const auto v = encoded.get<Vec3>();
  std::cout << "Vec3 round-trip: (" << v->x << ", " << v->y << ", " << v->z << ")\n";
}

void policy_choices() {
  std::cout << "\n--- pluggable UTF-8 / duplicate-key policies ---\n";

  // RFC 8949 Section 5.3.1's overlong-UTF-8 example: well-formed CBOR, invalid UTF-8.
  // Validation is a template parameter.
  constexpr std::uint8_t overlong_utf8[] = {0x62, 0xc0, 0xae};
  std::cout << "accepted under NoUtf8Validation (the default): "
            << decode_value(overlong_utf8).has_value() << '\n';
  std::cout << "accepted under CompleteUtf8Validation: "
            << decode_value<32, CompleteUtf8Validation>(overlong_utf8).has_value() << '\n';

  // {1: 2, 1: 3}: duplicate key; handling is the KeyPolicy template parameter.
  constexpr std::uint8_t duplicate_key_map[] = {0xa2, 0x01, 0x02, 0x01, 0x03};
  std::cout << "RejectDuplicateKeys (the default) refuses it: "
            << !decode_value(duplicate_key_map).has_value() << '\n';
  const auto kept_last = decode_value<32, NoUtf8Validation, KeepLastDuplicateKey>(duplicate_key_map);
  // Integer key: operator[] takes any key type.
  const std::uint64_t surviving_value = *kept_last->first[CborValue(std::uint64_t{1})].get<std::uint64_t>();
  std::cout << "KeepLastDuplicateKey's surviving value for key 1: " << surviving_value << " (expected 3)\n";
}

void inspect_a_decode_error() {
  std::cout << "\n--- inspecting a refusal's Error (reason + byte offset) ---\n";

  // [1, <truncated uint>]: the error carries the reason and the byte offset.
  constexpr std::uint8_t truncated[] = {0x83, 0x01, 0x18};
  const auto decoded = decode_value(truncated);
  if (!decoded) {
    const Error& err = decoded.error();
    std::cout << "refused: reason=" << static_cast<int>(err.reason) << " (TruncatedInput="
              << static_cast<int>(ErrorReason::TruncatedInput) << ") offset=" << err.offset << " (expected 2)\n";
  }
}

}  // namespace

int main() {
  decode_and_inspect();
  build_and_encode();
  user_type_hook();
  user_type_cbor_fields();
  policy_choices();
  inspect_a_decode_error();
  return 0;
}
