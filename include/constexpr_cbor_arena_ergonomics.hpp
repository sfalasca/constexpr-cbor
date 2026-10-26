#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "constexpr_cbor.hpp"
#include "constexpr_cbor_arena.hpp"

// Opt-in ergonomic sugar for the no-malloc Arena decoder, mirroring
// CborValue's own is_*()/operator[]/.get<T>()/iteration surface
// (constexpr_cbor.hpp) but read-only: Arena::Node/NodePool's accessor
// methods all need the pool passed explicitly (a bare Node is meaningless
// alone), so ArenaView bundles a (pool, node_index) pair the same way a
// CborValue reference bundles its own data directly, letting
// `view["a"][2]["b"]` chain without the caller re-threading `pool` at every
// step. ArenaBuilder is the one mutating piece: it writes new nodes into an
// *existing* caller-owned NodePool's remaining capacity, with every write
// returning std::optional (pool exhaustion is an ordinary, expected failure
// here, unlike the heap builder where std::vector growth can't fail).
//
// Deliberately out of scope: a mutable builder-style operator[] on
// ArenaView itself (it would need the exact same optional-returning
// grow/promote machinery as the heap builder, but duplicated under a
// completely different failure contract -- not a small addition once pool
// exhaustion is taken seriously), and brace-initializer construction (no
// array/map literal heuristic is needed here since ArenaBuilder's
// make_array/make_map calls are explicit and unambiguous already).
namespace ConstexprCbor::Arena {

// One dedicated, matching-capacity sentinel pool per <MaxNodes,MaxBytes>
// instantiation (a variable template, not a function-local static, for the
// same consteval-safety reason CborValue::null_sentinel is namespace-scope
// in constexpr_cbor.hpp) -- a default-constructed NodePool already holds
// an Undefined node at index 0 (Node's variant defaults to its first
// alternative, std::monostate), so nothing further needs to be written
// into it.
template <std::size_t MaxNodes, std::size_t MaxBytes>
inline constexpr NodePool<MaxNodes, MaxBytes> arena_sentinel_pool{};

template <std::size_t MaxNodes, std::size_t MaxBytes>
struct ArenaView {
  const NodePool<MaxNodes, MaxBytes>* pool;
  std::size_t node_index;

  constexpr const Node& node() const { return pool->resolve(node_index); }

  static constexpr ArenaView sentinel() { return ArenaView{&arena_sentinel_pool<MaxNodes, MaxBytes>, 0}; }

  constexpr bool is_undefined() const { return std::holds_alternative<std::monostate>(node().data); }
  constexpr bool is_null() const { return std::holds_alternative<std::nullptr_t>(node().data); }
  constexpr bool is_boolean() const { return std::holds_alternative<bool>(node().data); }
  constexpr bool is_unsigned() const { return std::holds_alternative<std::uint64_t>(node().data); }
  constexpr bool is_negative_int() const { return std::holds_alternative<NegativeInt>(node().data); }
  constexpr bool is_bignum() const { return std::holds_alternative<BigNumRef>(node().data); }
  constexpr bool is_integer() const { return is_unsigned() || is_negative_int() || is_bignum(); }
  constexpr bool is_float() const { return std::holds_alternative<double>(node().data); }
  constexpr bool is_number() const { return is_integer() || is_float(); }
  constexpr bool is_string() const {
    const auto* s = std::get_if<StringRef>(&node().data);
    return s != nullptr && s->kind == CborValue::StringKind::Text;
  }
  constexpr bool is_binary() const {
    const auto* s = std::get_if<StringRef>(&node().data);
    return s != nullptr && s->kind == CborValue::StringKind::Bytes;
  }
  constexpr bool is_simple() const { return std::holds_alternative<CborValue::SimpleValue>(node().data); }
  constexpr bool is_array() const { return std::holds_alternative<ArrayRef>(node().data); }
  constexpr bool is_map() const { return std::holds_alternative<MapRef>(node().data); }
  constexpr bool is_object() const { return is_map(); }
  constexpr bool is_tag() const { return std::holds_alternative<TagRef>(node().data); }

  constexpr std::size_t size() const {
    if (is_undefined() || is_null()) return 0;
    if (const auto* a = std::get_if<ArrayRef>(&node().data)) return a->count;
    if (const auto* m = std::get_if<MapRef>(&node().data)) return m->count;
    return 1;
  }
  constexpr bool empty() const { return size() == 0; }

  constexpr bool contains(const ArenaView& key) const {
    const auto* m = std::get_if<MapRef>(&node().data);
    if (!m) return false;
    for (std::size_t i = 0; i < m->count; ++i) {
      if (node_equal(*pool, pool->resolve_index(m->first_pair + 2 * i), key.node_index)) return true;
    }
    return false;
  }

  constexpr ArenaView operator[](std::size_t i) const {
    if (const auto* a = std::get_if<ArrayRef>(&node().data); a && i < a->count) {
      return ArenaView{pool, pool->resolve_index(a->first_child + i)};
    }
    return sentinel();
  }

  // Only meaningful when `key`'s node lives in this SAME pool -- node_equal
  // compares two indices into one pool, not across two independent pools.
  // For the common case of looking a decoded map up by a plain text key
  // (which doesn't already live in the pool), use the std::string_view
  // overload below instead, which compares string *content* directly and
  // needs no shared pool.
  constexpr ArenaView operator[](const ArenaView& key) const {
    if (const auto* m = std::get_if<MapRef>(&node().data)) {
      for (std::size_t i = 0; i < m->count; ++i) {
        if (node_equal(*pool, pool->resolve_index(m->first_pair + 2 * i), key.node_index)) {
          return ArenaView{pool, pool->resolve_index(m->first_pair + 2 * i + 1)};
        }
      }
    }
    return sentinel();
  }

  constexpr ArenaView operator[](std::string_view key) const {
    if (const auto* m = std::get_if<MapRef>(&node().data)) {
      for (std::size_t i = 0; i < m->count; ++i) {
        const std::size_t key_idx = pool->resolve_index(m->first_pair + 2 * i);
        const auto* s = std::get_if<StringRef>(&pool->nodes[key_idx].data);
        if (s && s->kind == CborValue::StringKind::Text) {
          const auto content = pool->bytes_of(*s);
          if (content.size() == key.size() &&
              std::equal(content.begin(), content.end(), key.begin(),
                         [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); })) {
            return ArenaView{pool, pool->resolve_index(m->first_pair + 2 * i + 1)};
          }
        }
      }
    }
    return sentinel();
  }

  constexpr bool contains(std::string_view key) const {
    const auto* m = std::get_if<MapRef>(&node().data);
    if (!m) return false;
    for (std::size_t i = 0; i < m->count; ++i) {
      const std::size_t key_idx = pool->resolve_index(m->first_pair + 2 * i);
      const auto* s = std::get_if<StringRef>(&pool->nodes[key_idx].data);
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
  constexpr std::optional<T> get() const {
    const Node& n = node();
    if constexpr (std::same_as<T, bool>) {
      const auto* p = std::get_if<bool>(&n.data);
      return p ? std::optional<T>(*p) : std::nullopt;
    } else if constexpr (std::same_as<T, std::uint64_t>) {
      const auto* p = std::get_if<std::uint64_t>(&n.data);
      return p ? std::optional<T>(*p) : std::nullopt;
    } else if constexpr (std::same_as<T, std::int64_t>) {
      if (const auto* p = std::get_if<std::uint64_t>(&n.data)) {
        if (*p <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return static_cast<std::int64_t>(*p);
        }
        return std::nullopt;
      }
      if (const auto* p = std::get_if<NegativeInt>(&n.data)) {
        return p->to_int64();
      }
      return std::nullopt;
    } else if constexpr (std::same_as<T, double>) {
      const auto* p = std::get_if<double>(&n.data);
      return p ? std::optional<T>(*p) : std::nullopt;
    } else if constexpr (std::same_as<T, std::string>) {
      const auto* p = std::get_if<StringRef>(&n.data);
      if (p && p->kind == CborValue::StringKind::Text) {
        const auto bytes = pool->bytes_of(*p);
        return std::string(bytes.begin(), bytes.end());
      }
      return std::nullopt;
    } else if constexpr (std::same_as<T, std::vector<std::uint8_t>>) {
      const auto* p = std::get_if<StringRef>(&n.data);
      if (p && p->kind == CborValue::StringKind::Bytes) {
        const auto bytes = pool->bytes_of(*p);
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
      }
      return std::nullopt;
    } else if constexpr (std::same_as<T, std::monostate>) {
      return is_undefined() ? std::optional<T>(std::monostate{}) : std::nullopt;
    } else if constexpr (std::same_as<T, std::nullptr_t>) {
      return is_null() ? std::optional<T>(nullptr) : std::nullopt;
    } else {
      static_assert(sizeof(T) == 0, "ArenaView::get<T>() has no built-in support for this T (no ADL hook support "
                                     "for Arena -- convert via the heap CborValue API instead if needed)");
    }
  }

  // Iteration: array yields elements in order; map yields values only, no
  // primitive iteration arm -- same rules as CborValue's own iterator.
  struct const_iterator {
    const ArenaView* owner;
    std::size_t index;
    constexpr ArenaView operator*() const {
      const Node& n = owner->node();
      if (const auto* a = std::get_if<ArrayRef>(&n.data)) {
        return ArenaView{owner->pool, owner->pool->resolve_index(a->first_child + index)};
      }
      const auto& m = std::get<MapRef>(n.data);
      return ArenaView{owner->pool, owner->pool->resolve_index(m.first_pair + 2 * index + 1)};
    }
    constexpr const_iterator& operator++() {
      ++index;
      return *this;
    }
    constexpr bool operator==(const const_iterator& other) const { return index == other.index; }
  };
  constexpr const_iterator begin() const { return const_iterator{this, 0}; }
  constexpr const_iterator end() const { return const_iterator{this, size()}; }

  struct ItemsProxy {
    const ArenaView* owner;
    struct Item {
      ArenaView key;
      ArenaView value;
    };
    struct iterator {
      const ArenaView* owner;
      std::size_t index;
      constexpr Item operator*() const {
        const auto& m = std::get<MapRef>(owner->node().data);
        return Item{ArenaView{owner->pool, owner->pool->resolve_index(m.first_pair + 2 * index)},
                    ArenaView{owner->pool, owner->pool->resolve_index(m.first_pair + 2 * index + 1)}};
      }
      constexpr iterator& operator++() {
        ++index;
        return *this;
      }
      constexpr bool operator==(const iterator& other) const { return index == other.index; }
    };
    constexpr iterator begin() const { return iterator{owner, 0}; }
    constexpr iterator end() const { return iterator{owner, owner->is_map() ? owner->size() : 0}; }
  };
  constexpr ItemsProxy items() const { return ItemsProxy{this}; }
};

template <std::size_t MaxNodes, std::size_t MaxBytes>
constexpr ArenaView<MaxNodes, MaxBytes> view(const NodePool<MaxNodes, MaxBytes>& pool, std::size_t node_index) {
  return ArenaView<MaxNodes, MaxBytes>{&pool, node_index};
}

// One-call ergonomic entry point: decode straight into a view.
template <std::size_t MaxDepth = 32, typename Utf8Policy = NoUtf8Validation, typename KeyPolicy = RejectDuplicateKeys,
          std::size_t MaxNodes, std::size_t MaxBytes>
constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> decode_view(NodePool<MaxNodes, MaxBytes>& pool,
                                                                    std::span<const std::uint8_t> bytes) {
  const auto decoded = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes);
  if (!decoded) return std::nullopt;
  return ArenaView<MaxNodes, MaxBytes>{&pool, decoded->node_index};
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
template <std::size_t MaxNodes, std::size_t MaxBytes>
struct ArenaBuilder {
  NodePool<MaxNodes, MaxBytes>* pool;

  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_null() { return alloc(Node{.data = nullptr}); }
  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_bool(bool v) { return alloc(Node{.data = v}); }
  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_uint(std::uint64_t v) { return alloc(Node{.data = v}); }
  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_int(std::int64_t v) {
    if (v >= 0) return make_uint(static_cast<std::uint64_t>(v));
    return alloc(Node{.data = NegativeInt{.n = static_cast<std::uint64_t>(-1 - v)}});
  }
  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_float(double v) { return alloc(Node{.data = v}); }

  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_text(std::string_view sv) {
    std::vector<std::uint8_t> buf(sv.size());
    for (std::size_t i = 0; i < sv.size(); ++i) {
      buf[i] = static_cast<std::uint8_t>(sv[i]);
    }
    const auto off = pool->allocate_bytes(buf);
    if (!off) return std::nullopt;
    return alloc(Node{.data = StringRef{.kind = CborValue::StringKind::Text, .offset = *off, .length = sv.size()}});
  }
  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_bytes(std::span<const std::uint8_t> bytes) {
    const auto off = pool->allocate_bytes(bytes);
    if (!off) return std::nullopt;
    return alloc(Node{.data = StringRef{.kind = CborValue::StringKind::Bytes, .offset = *off, .length = bytes.size()}});
  }

  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_array(
      std::span<const ArenaView<MaxNodes, MaxBytes>> children) {
    const auto first = pool->reserve_nodes(children.size());
    if (!first) return std::nullopt;
    for (std::size_t i = 0; i < children.size(); ++i) {
      pool->nodes[*first + i] = Node{.data = IndexCell{.actual_index = children[i].node_index}};
    }
    return alloc(Node{.data = ArrayRef{.first_child = *first, .count = children.size()}});
  }

  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> make_map(
      std::span<const std::pair<ArenaView<MaxNodes, MaxBytes>, ArenaView<MaxNodes, MaxBytes>>> pairs) {
    const auto first = pool->reserve_nodes(2 * pairs.size());
    if (!first) return std::nullopt;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
      pool->nodes[*first + 2 * i] = Node{.data = IndexCell{.actual_index = pairs[i].first.node_index}};
      pool->nodes[*first + 2 * i + 1] = Node{.data = IndexCell{.actual_index = pairs[i].second.node_index}};
    }
    return alloc(Node{.data = MapRef{.first_pair = *first, .count = pairs.size()}});
  }

 private:
  constexpr std::optional<ArenaView<MaxNodes, MaxBytes>> alloc(Node n) {
    const auto idx = pool->allocate_node(n);
    if (!idx) return std::nullopt;
    return ArenaView<MaxNodes, MaxBytes>{pool, *idx};
  }
};

}  // namespace ConstexprCbor::Arena
