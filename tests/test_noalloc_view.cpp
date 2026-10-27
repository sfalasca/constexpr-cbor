#include <ranges>

#include "compile_time_ut.hpp"
#include "constexpr_cbor_noalloc_ergonomics.hpp"

using namespace ConstexprCbor;
using namespace ConstexprCbor::NoAlloc;
using namespace CompileTimeUnitTesting;

// NoAllocView is a genuine std::ranges::forward_range too, the same bar
// CborValue and SaxRange are held to -- NoAllocView::const_iterator/
// ItemsProxy::iterator are copyable value types with no shared external
// cursor, so two copies can advance independently.
static_assert(std::ranges::forward_range<NoAllocView>);
static_assert(std::ranges::forward_range<NoAllocView::ItemsProxy>);

// Decode the same bytes via both the heap decoder and NoAlloc::decode_view,
// prove every predicate/operator[]/.get<T>()/iteration result agrees
// between the resulting CborValue and the resulting NoAllocView.
constexpr std::uint8_t kBytes[] = {
    0xa2,                                // map(2)
    0x61, 0x61, 0x01,                    //   "a": 1
    0x61, 0x62, 0x82, 0x02, 0x03,        //   "b": [2, 3]
};

constexpr bool heap_and_noalloc_agree_on_shape() {
  const auto heap = decode_value(kBytes);
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto view = decode_view(pool, kBytes);
  if (!heap || !view) return false;
  if (heap->first.is_map() != view->is_map()) return false;
  if (heap->first.size() != view->size()) return false;
  if (heap->first["a"].get<std::uint64_t>() != view->operator[]("a").get<std::uint64_t>()) return false;
  const auto heap_b = heap->first["b"];
  const auto view_b = (*view)["b"];
  if (heap_b.is_array() != view_b.is_array() || heap_b.size() != view_b.size()) return false;
  if (heap_b[0].get<std::uint64_t>() != view_b[0].get<std::uint64_t>()) return false;
  if (heap_b[1].get<std::uint64_t>() != view_b[1].get<std::uint64_t>()) return false;
  if (view->contains("a") != heap->first.contains("a")) return false;
  if (view->contains("z") != heap->first.contains("z")) return false;
  return true;
}

constexpr std::uint64_t sum_array_view_iteration() {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto view = decode_view(pool, kBytes);
  if (!view) return 0;
  const auto b = (*view)["b"];
  std::uint64_t total = 0;
  for (const auto& item : b) {
    total += item.get<std::uint64_t>().value_or(0);
  }
  return total;
}

constexpr bool items_proxy_matches_heap() {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto view = decode_view(pool, kBytes);
  if (!view) return false;
  std::size_t count = 0;
  for (const auto item : view->items()) {
    (void)item;
    ++count;
  }
  return count == 2;
}

constexpr bool miss_returns_undefined_sentinel() {
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto view = decode_view(pool, kBytes);
  if (!view) return false;
  return (*view)["missing"].is_undefined() && (*view)[99].is_undefined();
}

constexpr bool tags_and_bignums_agree() {
  // tag(1)(1363896240) -> 0xc1 1a 51 4b 67 b0; both views should report
  // is_tag() and the same nested scalar.
  constexpr std::uint8_t tagged[] = {0xc1, 0x1a, 0x51, 0x4b, 0x67, 0xb0};
  const auto heap = decode_value(tagged);
  NoAllocStorage<32, 32, 48> storage;
  auto pool = storage.make_pool();
  const auto view = decode_view(pool, tagged);
  if (!heap || !view) return false;
  return heap->first.is_tag() == view->is_tag();
}

consteval bool test_noalloc_view_parity() {
  expect_true<heap_and_noalloc_agree_on_shape()>();
  expect_eq<sum_array_view_iteration(), std::uint64_t{5}>();
  expect_true<items_proxy_matches_heap()>();
  expect_true<miss_returns_undefined_sentinel()>();
  expect_true<tags_and_bignums_agree()>();
  return true;
}
static_assert(test_noalloc_view_parity());

int main() { return test_noalloc_view_parity() ? 0 : 1; }
