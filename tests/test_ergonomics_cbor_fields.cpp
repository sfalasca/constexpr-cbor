#include "compile_time_ut.hpp"
#include "constexpr_cbor.hpp"

using namespace ConstexprCbor;
using namespace CompileTimeUnitTesting;

// A plain aggregate struct with no hand-written to_cbor/from_cbor at all --
// cbor_fields<Vec2>() is the only thing CborSerializer<Vec2>'s default body
// finds, and it's enough to build a working to_cbor/from_cbor pair.
namespace user_ns {
struct Vec2 {
  int x;
  int y;
  friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};
}  // namespace user_ns

namespace ConstexprCbor {
template <>
constexpr auto cbor_fields<user_ns::Vec2>() {
  return std::tuple{std::pair{"x", &user_ns::Vec2::x}, std::pair{"y", &user_ns::Vec2::y}};
}
}  // namespace ConstexprCbor

constexpr bool cbor_fields_round_trip() {
  const user_ns::Vec2 original{3, 4};
  const CborValue v = original;
  if (!v.is_map()) return false;
  const auto back = v.get<user_ns::Vec2>();
  return back.has_value() && *back == original;
}

// A struct whose own field is itself a cbor_fields-reflected type -- proves
// the generic implementation recurses through the same dispatch chain
// rather than only handling built-in field types.
namespace user_ns {
struct Segment {
  Vec2 from;
  Vec2 to;
  friend constexpr bool operator==(const Segment&, const Segment&) = default;
};
}  // namespace user_ns

namespace ConstexprCbor {
template <>
constexpr auto cbor_fields<user_ns::Segment>() {
  return std::tuple{std::pair{"from", &user_ns::Segment::from}, std::pair{"to", &user_ns::Segment::to}};
}
}  // namespace ConstexprCbor

constexpr bool cbor_fields_nested_round_trip() {
  const user_ns::Segment original{{1, 2}, {3, 4}};
  const CborValue v = original;
  const auto back = v.get<user_ns::Segment>();
  return back.has_value() && *back == original;
}

// A type with BOTH an ADL to_cbor/from_cbor pair and a cbor_fields<T>()
// specialization -- the ADL hook must win (checked by the ADL encoding
// producing an observably different shape: a bare int, not a map).
namespace user_ns {
struct Dual {
  int value;
};
constexpr bool to_cbor(CborValue& out, const Dual& d) {
  out = d.value;
  return true;
}
constexpr bool from_cbor(const CborValue& in, Dual& d) {
  const auto n = in.get<std::int64_t>();
  if (!n) return false;
  d.value = static_cast<int>(*n);
  return true;
}
}  // namespace user_ns

namespace ConstexprCbor {
template <>
constexpr auto cbor_fields<user_ns::Dual>() {
  return std::tuple{std::pair{"value", &user_ns::Dual::value}};
}
}  // namespace ConstexprCbor

constexpr bool adl_takes_precedence_over_cbor_fields() {
  const user_ns::Dual original{9};
  const CborValue v = original;
  if (v.is_map()) return false;  // cbor_fields would have produced a map
  const auto back = v.get<user_ns::Dual>();
  return back.has_value() && back->value == 9;
}

consteval bool test_cbor_fields() {
  expect_true<cbor_fields_round_trip()>();
  expect_true<cbor_fields_nested_round_trip()>();
  expect_true<adl_takes_precedence_over_cbor_fields()>();
  return true;
}
static_assert(test_cbor_fields());

int main() { return test_cbor_fields() ? 0 : 1; }
