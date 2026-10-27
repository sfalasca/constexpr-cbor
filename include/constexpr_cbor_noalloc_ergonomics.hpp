#pragma once

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>

#include "constexpr_cbor.hpp"
#include "constexpr_cbor_noalloc.hpp"

// Opt-in ergonomic sugar for the no-malloc NoAlloc decoder, mirroring
// CborValue's own is_*()/operator[]/.get<T>()/iteration surface
// (constexpr_cbor.hpp) but read-only: NoAlloc::Node's accessor methods still
// need the pool passed explicitly for structural operations (contains(),
// the string_view key overloads, and operator[](const NoAllocView&), all of
// which go through node_equal), so NoAllocView bundles a (pool, node) pair
// the same way a CborValue reference bundles its own data directly,
// letting `view["a"][2]["b"]` chain without the caller re-threading `pool`
// at every step. NoAllocBuilder is the one mutating piece: it writes new
// nodes into an *existing* caller-owned NodePool's remaining capacity,
// with every write returning std::optional (pool exhaustion is an
// ordinary, expected failure here, unlike the heap builder where
// std::vector growth can't fail).
//
// Deliberately out of scope: a mutable builder-style operator[] on
// NoAllocView itself (it would need the exact same optional-returning
// grow/promote machinery as the heap builder, but duplicated under a
// completely different failure contract -- not a small addition once pool
// exhaustion is taken seriously), and brace-initializer construction (no
// array/map literal heuristic is needed here since NoAllocBuilder's
// make_array/make_map calls are explicit and unambiguous already).
namespace ConstexprCbor::NoAlloc {

// A single, non-template sentinel node -- the pointer-world replacement
// for the old per-<MaxNodes,MaxBytes> sentinel pool variable template.
// NoAllocView::sentinel() needs no real pool at all: none of NoAllocView's
// read-only predicates/accessors ever dereference `pool` for an
// is_undefined() node, so the pool member can simply be nullptr.
inline constexpr Node undefined_sentinel_node{.data = std::monostate{}};

struct NoAllocView {
  const NodePool* pool;  // may be nullptr for a sentinel view -- see above
  const Node* node;

  static constexpr NoAllocView sentinel() { return NoAllocView{nullptr, &undefined_sentinel_node}; }

  constexpr bool is_undefined() const { return std::holds_alternative<std::monostate>(node->data); }
  constexpr bool is_null() const { return std::holds_alternative<std::nullptr_t>(node->data); }
  constexpr bool is_boolean() const { return std::holds_alternative<bool>(node->data); }
  constexpr bool is_unsigned() const { return std::holds_alternative<std::uint64_t>(node->data); }
  constexpr bool is_negative_int() const { return std::holds_alternative<NegativeInt>(node->data); }
  constexpr bool is_bignum() const { return std::holds_alternative<BigNumRef>(node->data); }
  constexpr bool is_integer() const { return is_unsigned() || is_negative_int() || is_bignum(); }
  constexpr bool is_float() const { return std::holds_alternative<double>(node->data); }
  constexpr bool is_number() const { return is_integer() || is_float(); }
  constexpr bool is_string() const {
    const auto* s = std::get_if<StringRef>(&node->data);
    return s != nullptr && s->kind == CborValue::StringKind::Text;
  }
  constexpr bool is_binary() const {
    const auto* s = std::get_if<StringRef>(&node->data);
    return s != nullptr && s->kind == CborValue::StringKind::Bytes;
  }
  constexpr bool is_simple() const { return std::holds_alternative<CborValue::SimpleValue>(node->data); }
  constexpr bool is_array() const { return std::holds_alternative<ArrayRef>(node->data); }
  constexpr bool is_map() const { return std::holds_alternative<MapRef>(node->data); }
  constexpr bool is_object() const { return is_map(); }
  constexpr bool is_tag() const { return std::holds_alternative<TagRef>(node->data); }

  constexpr std::size_t size() const {
    if (is_undefined() || is_null()) return 0;
    if (const auto* a = std::get_if<ArrayRef>(&node->data)) return a->count;
    if (const auto* m = std::get_if<MapRef>(&node->data)) return m->count;
    return 1;
  }
  constexpr bool empty() const { return size() == 0; }

  constexpr bool contains(const NoAllocView& key) const {
    const auto* m = std::get_if<MapRef>(&node->data);
    if (!m) return false;
    for (std::size_t i = 0; i < m->count; ++i) {
      if (node_equal(*pool, m->first_pair[2 * i], key.node)) return true;
    }
    return false;
  }

  constexpr NoAllocView operator[](std::size_t i) const {
    if (const auto* a = std::get_if<ArrayRef>(&node->data); a && i < a->count) {
      return NoAllocView{pool, a->first_child[i]};
    }
    return sentinel();
  }

  // Only meaningful when `key`'s node lives in this SAME pool -- node_equal
  // compares two nodes reachable from one pool's byte storage (for
  // StringRef/BigNumRef content comparison), not across two independent
  // pools. For the common case of looking a decoded map up by a plain
  // text key (which doesn't already live in the pool), use the
  // std::string_view overload below instead, which compares string
  // *content* directly and needs no shared pool.
  constexpr NoAllocView operator[](const NoAllocView& key) const {
    if (const auto* m = std::get_if<MapRef>(&node->data)) {
      for (std::size_t i = 0; i < m->count; ++i) {
        if (node_equal(*pool, m->first_pair[2 * i], key.node)) {
          return NoAllocView{pool, m->first_pair[2 * i + 1]};
        }
      }
    }
    return sentinel();
  }

  constexpr NoAllocView operator[](std::string_view key) const {
    if (const auto* m = std::get_if<MapRef>(&node->data)) {
      for (std::size_t i = 0; i < m->count; ++i) {
        const auto* s = std::get_if<StringRef>(&m->first_pair[2 * i]->data);
        if (s && s->kind == CborValue::StringKind::Text) {
          const auto content = pool->bytes_of(*s);
          if (content.size() == key.size() &&
              std::equal(content.begin(), content.end(), key.begin(),
                         [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); })) {
            return NoAllocView{pool, m->first_pair[2 * i + 1]};
          }
        }
      }
    }
    return sentinel();
  }

  constexpr bool contains(std::string_view key) const {
    const auto* m = std::get_if<MapRef>(&node->data);
    if (!m) return false;
    for (std::size_t i = 0; i < m->count; ++i) {
      const auto* s = std::get_if<StringRef>(&m->first_pair[2 * i]->data);
      if (s && s->kind == CborValue::StringKind::Text) {
        const auto content = pool->bytes_of(*s);
        if (content.size() == key.size() &&
            std::equal(content.begin(), content.end(), key.begin(),
                       [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); })) {
          return true;
        }
      }
    }
    return false;
  }

  template <typename T>
  constexpr Expected<T, Error> get() const {
    if constexpr (std::same_as<T, bool>) {
      const auto* p = std::get_if<bool>(&node->data);
      return p ? Expected<T, Error>(*p) : unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::uint64_t>) {
      const auto* p = std::get_if<std::uint64_t>(&node->data);
      return p ? Expected<T, Error>(*p) : unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::int64_t>) {
      if (const auto* p = std::get_if<std::uint64_t>(&node->data)) {
        if (*p <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return static_cast<std::int64_t>(*p);
        }
        return unexpected(Error{ErrorReason::IntegerOutOfRange});
      }
      if (const auto* p = std::get_if<NegativeInt>(&node->data)) {
        return p->to_int64();
      }
      return unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, double>) {
      const auto* p = std::get_if<double>(&node->data);
      return p ? Expected<T, Error>(*p) : unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::string>) {
      const auto* p = std::get_if<StringRef>(&node->data);
      if (p && p->kind == CborValue::StringKind::Text) {
        const auto bytes = pool->bytes_of(*p);
        return std::string(bytes.begin(), bytes.end());
      }
      return unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::vector<std::uint8_t>>) {
      const auto* p = std::get_if<StringRef>(&node->data);
      if (p && p->kind == CborValue::StringKind::Bytes) {
        const auto bytes = pool->bytes_of(*p);
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
      }
      return unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::monostate>) {
      return is_undefined() ? Expected<T, Error>(std::monostate{}) : unexpected(Error{ErrorReason::TypeMismatch});
    } else if constexpr (std::same_as<T, std::nullptr_t>) {
      return is_null() ? Expected<T, Error>(nullptr) : unexpected(Error{ErrorReason::TypeMismatch});
    } else {
      static_assert(sizeof(T) == 0, "NoAllocView::get<T>() has no built-in support for this T (no ADL hook support "
                                     "for NoAlloc -- convert via the heap CborValue API instead if needed)");
    }
  }

  // Iteration: array yields elements in order; map yields values only, no
  // primitive iteration arm -- same rules as CborValue's own iterator.
  //
  // const_iterator/ItemsProxy are only forward-declared here, not defined
  // inline: unlike CborValue (whose own iterator nests inside a template),
  // NoAllocView is a concrete, non-template type now, and a nested class
  // whose own fields hold NoAllocView BY VALUE (ItemsProxy::Item below)
  // needs NoAllocView to already be a complete type -- which it isn't yet,
  // this early in its own body. Forward-declare, then give each its full
  // out-of-line definition (qualified as NoAllocView::const_iterator etc.)
  // right after this struct closes, where NoAllocView is complete.
  struct const_iterator;
  struct ItemsProxy;

  constexpr const_iterator begin() const;
  constexpr const_iterator end() const;
  constexpr ItemsProxy items() const;
};

struct NoAllocView::const_iterator {
  using value_type = NoAllocView;
  using difference_type = std::ptrdiff_t;
  using iterator_category = std::forward_iterator_tag;

  const NoAllocView* owner;
  std::size_t index;
  constexpr NoAllocView operator*() const {
    if (const auto* a = std::get_if<ArrayRef>(&owner->node->data)) {
      return NoAllocView{owner->pool, a->first_child[index]};
    }
    const auto& m = std::get<MapRef>(owner->node->data);
    return NoAllocView{owner->pool, m.first_pair[2 * index + 1]};
  }
  constexpr const_iterator& operator++() {
    ++index;
    return *this;
  }
  constexpr const_iterator operator++(int) {
    const_iterator tmp = *this;
    ++*this;
    return tmp;
  }
  constexpr bool operator==(const const_iterator& other) const { return index == other.index; }
};

struct NoAllocView::ItemsProxy {
  const NoAllocView* owner;
  struct Item {
    NoAllocView key;
    NoAllocView value;
  };
  struct iterator {
    using value_type = Item;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    const NoAllocView* owner;
    std::size_t index;
    constexpr Item operator*() const {
      const auto& m = std::get<MapRef>(owner->node->data);
      return Item{NoAllocView{owner->pool, m.first_pair[2 * index]},
                  NoAllocView{owner->pool, m.first_pair[2 * index + 1]}};
    }
    constexpr iterator& operator++() {
      ++index;
      return *this;
    }
    constexpr iterator operator++(int) {
      iterator tmp = *this;
      ++*this;
      return tmp;
    }
    constexpr bool operator==(const iterator& other) const { return index == other.index; }
  };
  constexpr iterator begin() const { return iterator{owner, 0}; }
  constexpr iterator end() const { return iterator{owner, owner->is_map() ? owner->size() : 0}; }
};

constexpr NoAllocView::const_iterator NoAllocView::begin() const { return const_iterator{this, 0}; }
constexpr NoAllocView::const_iterator NoAllocView::end() const { return const_iterator{this, size()}; }
constexpr NoAllocView::ItemsProxy NoAllocView::items() const { return ItemsProxy{this}; }

constexpr NoAllocView view(const NodePool& pool, const Node* node) { return NoAllocView{&pool, node}; }

// One-call ergonomic entry point: decode straight into a view.
template <std::size_t MaxDepth = 32, typename Utf8Policy = NoUtf8Validation, typename KeyPolicy = RejectDuplicateKeys>
constexpr Expected<NoAllocView, Error> decode_view(NodePool& pool, std::span<const std::uint8_t> bytes) {
  const auto decoded = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes);
  if (!decoded) return unexpected(decoded.error());
  return NoAllocView{&pool, decoded->node};
}

// Minimal builder: writes new nodes into an existing, caller-owned pool's
// remaining capacity. Every write returns std::optional -- pool exhaustion
// is an ordinary, expected failure here (applying the same rigor
// checked_node_count already applies to decode), not a crash. Children
// passed to make_array/make_map must already be built (post-order) views
// into the SAME pool; make_map does no duplicate-key handling (unlike
// decode, which is policy-templated against untrusted wire input, the
// builder just writes already-assembled pairs in order -- a caller needing
// dedup resolves it in their own pair list first).
struct NoAllocBuilder {
  NodePool* pool;

  constexpr Expected<NoAllocView, Error> make_null() { return alloc(Node{.data = nullptr}); }
  constexpr Expected<NoAllocView, Error> make_bool(bool v) { return alloc(Node{.data = v}); }
  constexpr Expected<NoAllocView, Error> make_uint(std::uint64_t v) { return alloc(Node{.data = v}); }
  constexpr Expected<NoAllocView, Error> make_int(std::int64_t v) {
    if (v >= 0) return make_uint(static_cast<std::uint64_t>(v));
    return alloc(Node{.data = NegativeInt{.n = static_cast<std::uint64_t>(-1 - v)}});
  }
  constexpr Expected<NoAllocView, Error> make_float(double v) { return alloc(Node{.data = v}); }

  constexpr Expected<NoAllocView, Error> make_text(std::string_view sv) {
    std::vector<std::uint8_t> buf(sv.size());
    for (std::size_t i = 0; i < sv.size(); ++i) {
      buf[i] = static_cast<std::uint8_t>(sv[i]);
    }
    const auto data = pool->allocate_bytes(buf);
    if (!data) return unexpected(Error{ErrorReason::ByteCapacityExhausted});
    return alloc(Node{.data = StringRef{.kind = CborValue::StringKind::Text, .data = data, .length = sv.size()}});
  }
  constexpr Expected<NoAllocView, Error> make_bytes(std::span<const std::uint8_t> bytes) {
    const auto data = pool->allocate_bytes(bytes);
    if (!data) return unexpected(Error{ErrorReason::ByteCapacityExhausted});
    return alloc(Node{.data = StringRef{.kind = CborValue::StringKind::Bytes, .data = data, .length = bytes.size()}});
  }

  constexpr Expected<NoAllocView, Error> make_array(std::span<const NoAllocView> children) {
    Node** first = pool->reserve_refs(children.size());
    if (!first) return unexpected(Error{ErrorReason::RefCapacityExhausted});
    for (std::size_t i = 0; i < children.size(); ++i) {
      NOALLOC_ASSERT(pool->owns(children[i].node));  // catches a child view from a different pool
      first[i] = const_cast<Node*>(children[i].node);
    }
    return alloc(Node{.data = ArrayRef{.first_child = first, .count = children.size()}});
  }

  constexpr Expected<NoAllocView, Error> make_map(std::span<const std::pair<NoAllocView, NoAllocView>> pairs) {
    Node** first = pool->reserve_refs(2 * pairs.size());
    if (!first) return unexpected(Error{ErrorReason::RefCapacityExhausted});
    for (std::size_t i = 0; i < pairs.size(); ++i) {
      NOALLOC_ASSERT(pool->owns(pairs[i].first.node));
      NOALLOC_ASSERT(pool->owns(pairs[i].second.node));
      first[2 * i] = const_cast<Node*>(pairs[i].first.node);
      first[2 * i + 1] = const_cast<Node*>(pairs[i].second.node);
    }
    return alloc(Node{.data = MapRef{.first_pair = first, .count = pairs.size()}});
  }

 private:
  constexpr Expected<NoAllocView, Error> alloc(Node n) {
    Node* node = pool->allocate_node(n);
    if (!node) return unexpected(Error{ErrorReason::NodeCapacityExhausted});
    return NoAllocView{pool, node};
  }
};

}  // namespace ConstexprCbor::NoAlloc
