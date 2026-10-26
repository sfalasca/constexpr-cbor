#include "compile_time_ut.hpp"
#include "constexpr_cbor_arena_ergonomics.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Arena;
using namespace CompileTimeUnitTesting;

// Build [1, [2, 3]] bottom-up, encode it, and compare against the
// canonical bytes 0x82 01 82 02 03.
constexpr bool nested_build_round_trips() {
  NodePool<32, 32> pool;
  ArenaBuilder<32, 32> b{&pool};
  const auto two = b.make_uint(2);
  const auto three = b.make_uint(3);
  if (!two || !three) return false;
  const ArenaView<32, 32> inner_children[] = {*two, *three};
  const auto inner = b.make_array(inner_children);
  if (!inner) return false;
  const auto one = b.make_uint(1);
  if (!one) return false;
  const ArenaView<32, 32> outer_children[] = {*one, *inner};
  const auto outer = b.make_array(outer_children);
  if (!outer) return false;

  std::array<std::uint8_t, 16> out{};
  const auto written = encode_value(pool, outer->node_index, out);
  if (!written) return false;
  constexpr std::uint8_t expected[] = {0x82, 0x01, 0x82, 0x02, 0x03};
  if (*written != sizeof(expected)) return false;
  for (std::size_t i = 0; i < sizeof(expected); ++i) {
    if (out[i] != expected[i]) return false;
  }
  return true;
}

constexpr bool text_and_map_round_trip() {
  NodePool<32, 32> pool;
  ArenaBuilder<32, 32> b{&pool};
  const auto key = b.make_text("a");
  const auto value = b.make_uint(1);
  if (!key || !value) return false;
  const std::pair<ArenaView<32, 32>, ArenaView<32, 32>> pairs[] = {{*key, *value}};
  const auto map = b.make_map(pairs);
  if (!map) return false;
  if (!map->is_map() || map->size() != 1) return false;
  return map->operator[]("a").get<std::uint64_t>() == 1;
}

// Pool exhaustion must be refused (nullopt), never UB/overflow -- the same
// rigor checked_node_count already applies to decode.
constexpr bool exhaustion_is_refused_not_ub() {
  NodePool<2, 16> pool;  // exactly 2 node slots
  ArenaBuilder<2, 16> b{&pool};
  const auto one = b.make_uint(1);
  const auto two = b.make_uint(2);
  if (!one || !two) return false;  // pool now exactly full
  const auto three = b.make_uint(3);
  return !three.has_value();
}

constexpr bool array_reservation_exhaustion_is_refused() {
  NodePool<3, 16> pool;  // room for 2 scalars + the array node, no more
  ArenaBuilder<3, 16> b{&pool};
  const auto one = b.make_uint(1);
  const auto two = b.make_uint(2);
  if (!one || !two) return false;
  const ArenaView<3, 16> children[] = {*one, *two};
  // Needs 2 more IndexCell slots (for the children) plus 1 for the ArrayRef
  // node itself = 3 more, but only 1 slot remains.
  const auto arr = b.make_array(children);
  return !arr.has_value();
}

consteval bool test_arena_builder() {
  expect_true<nested_build_round_trips()>();
  expect_true<text_and_map_round_trip()>();
  expect_true<exhaustion_is_refused_not_ub()>();
  expect_true<array_reservation_exhaustion_is_refused()>();
  return true;
}
static_assert(test_arena_builder());

int main() { return test_arena_builder() ? 0 : 1; }
