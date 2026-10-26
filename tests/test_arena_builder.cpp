#include "compile_time_ut.hpp"
#include "constexpr_cbor_arena_ergonomics.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::Arena;
using namespace CompileTimeUnitTesting;

// Build [1, [2, 3]] bottom-up, encode it, and compare against the
// canonical bytes 0x82 01 82 02 03.
constexpr bool nested_build_round_trips() {
  ArenaStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  ArenaBuilder b{&pool};
  const auto two = b.make_uint(2);
  const auto three = b.make_uint(3);
  if (!two || !three) return false;
  const ArenaView inner_children[] = {*two, *three};
  const auto inner = b.make_array(inner_children);
  if (!inner) return false;
  const auto one = b.make_uint(1);
  if (!one) return false;
  const ArenaView outer_children[] = {*one, *inner};
  const auto outer = b.make_array(outer_children);
  if (!outer) return false;

  std::array<std::uint8_t, 16> out{};
  const auto written = encode_value(pool, outer->node, out);
  if (!written) return false;
  constexpr std::uint8_t expected[] = {0x82, 0x01, 0x82, 0x02, 0x03};
  if (*written != sizeof(expected)) return false;
  for (std::size_t i = 0; i < sizeof(expected); ++i) {
    if (out[i] != expected[i]) return false;
  }
  return true;
}

constexpr bool text_and_map_round_trip() {
  ArenaStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  ArenaBuilder b{&pool};
  const auto key = b.make_text("a");
  const auto value = b.make_uint(1);
  if (!key || !value) return false;
  const std::pair<ArenaView, ArenaView> pairs[] = {{*key, *value}};
  const auto map = b.make_map(pairs);
  if (!map) return false;
  if (!map->is_map() || map->size() != 1) return false;
  return map->operator[]("a").get<std::uint64_t>() == 1;
}

// Pool exhaustion must be refused (nullopt), never UB/overflow -- the same
// rigor checked_node_count already applies to decode.
constexpr bool exhaustion_is_refused_not_ub() {
  ArenaStorage<2, 2, 16> storage;  // exactly 2 node slots
  auto pool = storage.make_pool();
  ArenaBuilder b{&pool};
  const auto one = b.make_uint(1);
  const auto two = b.make_uint(2);
  if (!one || !two) return false;  // pool now exactly full
  const auto three = b.make_uint(3);
  return !three.has_value();
}

// Ref-slot exhaustion is its own, independently-exhaustible axis now (see
// test_arena.cpp's test_decode_refuses_when_ref_pool_exhausted for the
// decode-side equivalent): node capacity is generous, but only 1 of the 2
// reference slots make_array needs for its two children is available.
constexpr bool array_reservation_exhaustion_is_refused() {
  ArenaStorage<3, 1, 16> storage;  // 3 node slots, but only 1 ref slot
  auto pool = storage.make_pool();
  ArenaBuilder b{&pool};
  const auto one = b.make_uint(1);
  const auto two = b.make_uint(2);
  if (!one || !two) return false;
  const ArenaView children[] = {*one, *two};
  const auto arr = b.make_array(children);  // needs 2 ref slots, only 1 available
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
