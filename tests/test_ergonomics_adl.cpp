#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;
using namespace CompileTimeUnitTesting;

// A user struct with free to_cbor/from_cbor in its OWN namespace, found via
// ADL with no `using` declaration anywhere -- the primary escape hatch.
namespace user_ns {
struct Point {
  int x;
  int y;
  friend constexpr bool operator==(const Point&, const Point&) = default;
};

constexpr bool to_cbor(CborValue& out, const Point& p) {
  out = CborValue::object({{"x", p.x}, {"y", p.y}});
  return true;
}
constexpr bool from_cbor(const CborValue& in, Point& p) {
  const auto x = in["x"].get<std::int64_t>();
  const auto y = in["y"].get<std::int64_t>();
  if (!x || !y) return false;
  p.x = static_cast<int>(*x);
  p.y = static_cast<int>(*y);
  return true;
}
}  // namespace user_ns

constexpr bool adl_round_trip() {
  const user_ns::Point original{3, 4};
  const CborValue v = original;
  if (!v.is_map()) return false;
  const auto back = v.get<user_ns::Point>();
  return back.has_value() && *back == original;
}

// A type the user can't attach a free function to at all (modeled here as
// a type the test deliberately doesn't control, inside an unrelated
// "third_party" namespace with no to_cbor/from_cbor of its own) -- the
// escape hatch is specializing CborSerializer<T> directly instead.
namespace third_party {
struct Opaque {
  int value;
};
}  // namespace third_party

namespace ConstexprCbor {
template <>
struct CborSerializer<third_party::Opaque> {
  static constexpr bool to_cbor(CborValue& out, const third_party::Opaque& v) {
    out = v.value;
    return true;
  }
  static constexpr bool from_cbor(const CborValue& in, third_party::Opaque& v) {
    const auto n = in.get<std::uint64_t>();
    if (!n) return false;
    v.value = static_cast<int>(*n);
    return true;
  }
};
}  // namespace ConstexprCbor

constexpr bool specialization_round_trip() {
  const third_party::Opaque original{7};
  const CborValue v = original;
  const auto back = v.get<third_party::Opaque>();
  return back.has_value() && back->value == 7;
}

consteval bool test_adl() {
  expect_true<adl_round_trip()>();
  expect_true<specialization_round_trip()>();
  return true;
}
static_assert(test_adl());

int main() { return test_adl() ? 0 : 1; }
