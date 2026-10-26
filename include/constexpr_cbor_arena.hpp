#pragma once

#include <algorithm>
#include <cassert>

#include "constexpr_cbor.hpp"

// assert() is used deliberately, not a bespoke contract-check helper: it
// never throws (abort()s directly, so it's -fno-exceptions-compatible),
// NDEBUG strips it in release builds, and inside a consteval/constexpr
// evaluation a *failing* assert() simply makes the enclosing expression
// not a core constant expression -- GCC/Clang report that as an ordinary
// compile error, which is exactly the desired "canary violated => hard
// failure" behavior. It never actually fires on the legitimate decode
// path (nothing stomps the caller's arrays mid-evaluation); it exists for
// the genuine-misuse case NodePool::canaries_intact() documents.
#define ARENA_ASSERT(cond) assert(cond)

// ---------------------------------------------------------------------------
// No-malloc counterpart of constexpr_cbor.hpp, for environments where a heap
// (and therefore std::vector) isn't available at all. CborValue's own design
// relies on std::vector<CborValue> permitting an incomplete element type
// (for the recursive Array/Map/Tag alternatives); std::array<T,N> has no
// such allowance, so a self-referential type cannot be stored inline in a
// fixed-size array no matter how it's declared -- some indirection is
// unavoidable. Here that indirection is a pointer into a Node arena the
// caller owns, rather than a heap pointer: every decoded value (including
// every array/map element and every tag's nested value) is a Node living
// in caller-supplied storage, and a node never contains another node by
// value, only by pointer.
//
// The caller owns three things: a Node[] (every scalar and composite node),
// a Node*[] (reference slots -- see ArrayRef/MapRef below for why these are
// a separate category from Node[] itself), and a byte[] (the flat backing
// store for every string/bignum's content). NodePool is a small, non-
// owning, trivially-copyable VIEW over those three caller-owned spans --
// never itself the thing that owns storage, the same relationship
// std::span has to the container it views. This is a deliberate split: the
// one thing this whole design depends on is that the caller's buffers
// don't move or get destroyed while any decoded tree pointing into them is
// still alive (every Node* this file ever hands back is a raw, unchecked
// pointer into that caller storage) -- and separating "the view" from "the
// storage" means the view can be freely copied/returned/reassigned without
// that ever being a concern, while the storage itself is the caller's own,
// ordinary local/static object, whose lifetime the caller already
// understands and controls (a std::array on their stack, a static, a
// member of a longer-lived object -- anything with a stable address for as
// long as the decoded tree is used). See NodePool::canaries_intact() below
// for a defensive, best-effort runtime check of that precondition -- it is
// not, and cannot be, a substitute for the caller actually upholding it.
//
// This file #includes constexpr_cbor.hpp rather than duplicating its
// shared, non-allocating primitives (decode_length, decode_uint/negint,
// the UTF-8 policies, decode_byte_string/decode_text_string, decode_simple)
// or its duplicate-key policy types: RejectDuplicateKeys/KeepFirstDuplicateKey/
// KeepLastDuplicateKey are the exact same types usable with both
// ConstexprCbor::decode_value and ConstexprCbor::Arena::decode_value (see
// their Arena-shaped on_duplicate overload in constexpr_cbor.hpp), not a
// parallel Arena-only copy of each -- "it's still a policy" means one
// coherent type per policy. A project that only wants the heap-based API
// includes just constexpr_cbor.hpp and never pays for any of this (every
// symbol below is a template, and templates cost nothing unless
// instantiated); a project that only wants the no-malloc API includes just
// this file, which pulls in constexpr_cbor.hpp transitively.
namespace ConstexprCbor::Arena {

struct StringRef {
  CborValue::StringKind kind;
  const std::uint8_t* data;
  std::size_t length;
  friend constexpr bool operator==(const StringRef&, const StringRef&) = default;
};

struct BigNumRef {
  bool negative;
  const std::uint8_t* data;
  std::size_t length;
  friend constexpr bool operator==(const BigNumRef&, const BigNumRef&) = default;
};

struct ArrayRef {
  Node* const* first_child;  // count contiguous Node* reference slots
  std::size_t count;
  friend constexpr bool operator==(const ArrayRef&, const ArrayRef&) = default;
};

struct MapRef {
  Node* const* first_pair;  // 2*count contiguous Node* slots: key, value, key, value, ...
  std::size_t count;
  friend constexpr bool operator==(const MapRef&, const MapRef&) = default;
};

struct TagRef {
  std::uint64_t number;
  Node* child;  // direct pointer -- no reference slot needed, a tag has exactly one child
  friend constexpr bool operator==(const TagRef&, const TagRef&) = default;
};

struct Node {
  std::variant<std::monostate, std::nullptr_t, bool, std::uint64_t, NegativeInt, double, StringRef,
               BigNumRef, CborValue::SimpleValue, ArrayRef, MapRef, TagRef>
      data;
  friend constexpr bool operator==(const Node&, const Node&) = default;
};

namespace detail {
// Two distinct magic words bracketing the caller's byte storage (see
// NodePool::canaries_intact()) -- distinct so a canary found at the wrong
// end (e.g. the storage span got reversed/aliased) is also detectable, not
// just "present or absent."
inline constexpr std::uint64_t byte_canary_begin = 0x4152454e41424f46ull;  // "ARENABOF"
inline constexpr std::uint64_t byte_canary_end = 0x4152454e41454f46ull;   // "ARENAEOF"
constexpr std::size_t canary_bytes = sizeof(std::uint64_t);
}  // namespace detail

// A small, non-owning, trivially-copyable VIEW over three caller-owned
// spans -- see this file's own top-of-file comment for the ownership split
// this embodies and the precondition it rests on (the caller's storage
// must not move or die while any decoded tree into it is alive).
//
// node_storage holds every scalar and composite Node this value's whole
// decoded tree may need; ref_storage holds the flat Node* reference slots
// array/map children are indexed through (see ArrayRef/MapRef above);
// byte_storage backs every string/bignum's content, with
// detail::canary_bytes reserved at each end for NodePool::canaries_intact()
// (see its own comment). Running out of any of the three during decode is
// a refusal, not a crash -- exactly like every other contract violation in
// this codec.
struct NodePool {
  Node* nodes_ = nullptr;
  std::size_t node_capacity_ = 0;
  std::size_t node_count_ = 0;

  Node** refs_ = nullptr;
  std::size_t ref_capacity_ = 0;
  std::size_t ref_count_ = 0;

  std::uint8_t* bytes_begin_ = nullptr;   // usable region starts after the begin canary
  std::size_t byte_capacity_ = 0;         // usable capacity, canaries already excluded
  std::size_t byte_count_ = 0;
  std::uint8_t* canary_begin_ = nullptr;  // nullptr if byte_storage was too small to hold both canaries
  std::uint8_t* canary_end_ = nullptr;

  constexpr NodePool(std::span<Node> node_storage, std::span<Node*> ref_storage,
                      std::span<std::uint8_t> byte_storage)
      : nodes_(node_storage.data()),
        node_capacity_(node_storage.size()),
        refs_(ref_storage.data()),
        ref_capacity_(ref_storage.size()) {
    if (byte_storage.size() >= 2 * detail::canary_bytes) {
      canary_begin_ = byte_storage.data();
      canary_end_ = byte_storage.data() + byte_storage.size() - detail::canary_bytes;
      write_canary(canary_begin_, detail::byte_canary_begin);
      write_canary(canary_end_, detail::byte_canary_end);
      bytes_begin_ = byte_storage.data() + detail::canary_bytes;
      byte_capacity_ = byte_storage.size() - 2 * detail::canary_bytes;
    }
    // byte_storage smaller than two canaries' worth: byte_capacity_ stays
    // 0 (every allocate_bytes call with a non-empty source simply refuses,
    // same as ordinary exhaustion), and canaries_intact() treats "no
    // canaries were ever written" as vacuously true -- there is nothing
    // to protect in storage too small to hold the guard itself.
  }

  constexpr Node* allocate_node(Node n) {
    ARENA_ASSERT(canaries_intact());
    if (node_count_ >= node_capacity_) return nullptr;
    nodes_[node_count_] = n;
    return &nodes_[node_count_++];
  }

  // Reserves `count` contiguous reference slots (left uninitialized --
  // every caller of this immediately overwrites each one before anyone
  // reads it back), returning a pointer to the first one, or nullptr if
  // they wouldn't fit. This is the slot array ArrayRef/MapRef index
  // through; it is NOT a Node allocation, which is the whole point of
  // this design -- see this file's own top comment.
  constexpr Node** reserve_refs(std::size_t count) {
    ARENA_ASSERT(canaries_intact());
    if (count > ref_capacity_ - ref_count_) return nullptr;
    Node** first = &refs_[ref_count_];
    ref_count_ += count;
    return first;
  }

  constexpr const std::uint8_t* allocate_bytes(std::span<const std::uint8_t> src) {
    ARENA_ASSERT(canaries_intact());
    if (src.size() > byte_capacity_ - byte_count_) return nullptr;
    std::uint8_t* dest = bytes_begin_ + byte_count_;
    // std::copy over a contiguous range of a trivial type lowers to
    // memmove at -O2 (unlike the equivalent index-and-increment loop,
    // which a compiler is free to but isn't guaranteed to recognize the
    // same way); std::copy is constexpr since C++20, so this stays valid
    // in the consteval decode paths too.
    std::copy(src.begin(), src.end(), dest);
    byte_count_ += src.size();
    return dest;
  }

  constexpr std::span<const std::uint8_t> bytes_of(const StringRef& ref) const {
    return std::span<const std::uint8_t>(ref.data, ref.length);
  }
  constexpr std::span<const std::uint8_t> bytes_of(const BigNumRef& ref) const {
    return std::span<const std::uint8_t>(ref.data, ref.length);
  }

  constexpr const Node& child(const ArrayRef& ref, std::size_t i) const { return *ref.first_child[i]; }
  constexpr const Node& map_key(const MapRef& ref, std::size_t i) const { return *ref.first_pair[2 * i]; }
  constexpr const Node& map_value(const MapRef& ref, std::size_t i) const {
    return *ref.first_pair[2 * i + 1];
  }
  constexpr const Node& tag_child(const TagRef& ref) const { return *ref.child; }

  // Best-effort, debug-only (NDEBUG-stripped, like the plain assert() it
  // wraps) defensive check that the caller's storage still looks like what
  // this pool was built over: the byte-storage canaries are still exactly
  // where they were written, and the node/ref counters are still within
  // their own capacities. This catches common misuse -- the caller's
  // buffer being reused for something else, a stale/moved-from NodePool,
  // accidental corruption -- but it is NOT a general dangling-buffer
  // detector: a buffer that has genuinely gone out of scope and whose
  // memory now happens to hold something that never touches the canary
  // words or exceeds the counts is ordinary, uncatchable UB, same as
  // dereferencing any other dangling pointer. The caller upholding "the
  // buffer outlives every decoded tree into it" is a precondition this
  // function can help catch violations of, not a guarantee it establishes.
  constexpr bool canaries_intact() const {
    if (node_count_ > node_capacity_) return false;
    if (ref_count_ > ref_capacity_) return false;
    if (canary_begin_ == nullptr) return true;  // storage too small to hold canaries at all -- see ctor
    return read_canary(canary_begin_) == detail::byte_canary_begin &&
           read_canary(canary_end_) == detail::byte_canary_end;
  }

 private:
  static constexpr void write_canary(std::uint8_t* dest, std::uint64_t value) {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      dest[i] = static_cast<std::uint8_t>(value >> (8 * (sizeof(value) - 1 - i)));
    }
  }
  static constexpr std::uint64_t read_canary(const std::uint8_t* src) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      value = (value << 8) | src[i];
    }
    return value;
  }
};

// Owning storage for a NodePool: the direct successor of this file's
// earlier NTTP-sized NodePool<MaxNodes,MaxBytes>, now explicitly "the
// buffer" rather than "the allocator" (NodePool above is the allocator --
// see the split explained in this file's top comment). MaxRefs is an
// independently-tunable third capacity, distinct from MaxNodes: every node
// that's ever an array/map child needs exactly one reference slot, so
// MaxRefs == MaxNodes is a safe, conservative default, but a caller
// decoding shallowly-nested, mostly-scalar data can size it much smaller.
template <std::size_t MaxNodes, std::size_t MaxRefs, std::size_t MaxBytes>
struct ArenaStorage {
  std::array<Node, MaxNodes> nodes{};
  std::array<Node*, MaxRefs> refs{};
  std::array<std::uint8_t, MaxBytes> bytes{};

  constexpr NodePool make_pool() { return NodePool(nodes, refs, bytes); }
};

// Walks exactly one CBOR item structurally (definite or indefinite form)
// and reports how many bytes it occupies, without allocating anything.
// Used only to pre-count an indefinite-length array/map's items before
// reserving their contiguous slot, since that reservation has to happen
// before any child is actually decoded. Deliberately duplicates some of
// decode_value's own structure rather than sharing it: unifying "count
// bytes only" with "materialize a value" into one templated walk would
// add real complexity for a marginal gain here.
template <std::size_t MaxDepth = 32>
constexpr std::optional<std::size_t> skip_item(std::span<const std::uint8_t> bytes, std::size_t depth = 0) {
  if (bytes.empty()) return std::nullopt;
  const std::uint8_t major_type = bytes[0] >> 5;
  switch (major_type) {
    case 0:
    case 1: {
      const auto len = decode_length(bytes);
      return len ? std::optional<std::size_t>(len->bytes_consumed) : std::nullopt;
    }
    case 2:
    case 3: {
      if (is_indefinite_or_break(bytes)) {
        std::size_t consumed = 1;
        while (true) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) return consumed + 1;
          const auto chunk_len = decode_length(bytes.subspan(consumed));
          if (!chunk_len) return std::nullopt;
          if (bytes.size() < consumed + chunk_len->bytes_consumed + chunk_len->value) return std::nullopt;
          consumed += chunk_len->bytes_consumed + chunk_len->value;
        }
      }
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      if (bytes.size() < len->bytes_consumed + len->value) return std::nullopt;
      return len->bytes_consumed + len->value;
    }
    case 4: {
      if (depth >= MaxDepth) return std::nullopt;
      std::size_t consumed = 1;
      if (is_indefinite_or_break(bytes)) {
        while (true) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) return consumed + 1;
          const auto item_len = skip_item<MaxDepth>(bytes.subspan(consumed), depth + 1);
          if (!item_len) return std::nullopt;
          consumed += *item_len;
        }
      }
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      consumed = len->bytes_consumed;
      for (std::uint64_t i = 0; i < len->value; ++i) {
        if (consumed > bytes.size()) return std::nullopt;
        const auto item_len = skip_item<MaxDepth>(bytes.subspan(consumed), depth + 1);
        if (!item_len) return std::nullopt;
        consumed += *item_len;
      }
      return consumed;
    }
    case 5: {
      if (depth >= MaxDepth) return std::nullopt;
      std::size_t consumed = 1;
      const bool indefinite = is_indefinite_or_break(bytes);
      std::optional<std::uint64_t> pair_count;
      if (!indefinite) {
        const auto len = decode_length(bytes);
        if (!len) return std::nullopt;
        consumed = len->bytes_consumed;
        pair_count = len->value;
      }
      std::uint64_t pairs_read = 0;
      while (indefinite ? true : pairs_read < *pair_count) {
        if (indefinite) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) return consumed + 1;
        }
        const auto key_len = skip_item<MaxDepth>(bytes.subspan(consumed), depth + 1);
        if (!key_len) return std::nullopt;
        consumed += *key_len;
        const auto value_len = skip_item<MaxDepth>(bytes.subspan(consumed), depth + 1);
        if (!value_len) return std::nullopt;
        consumed += *value_len;
        ++pairs_read;
      }
      return consumed;
    }
    case 6: {
      if (depth >= MaxDepth) return std::nullopt;
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      const auto nested_len = skip_item<MaxDepth>(bytes.subspan(len->bytes_consumed), depth + 1);
      if (!nested_len) return std::nullopt;
      return len->bytes_consumed + *nested_len;
    }
    case 7: {
      const auto s = decode_simple(bytes);
      return s ? std::optional<std::size_t>(s->bytes_consumed) : std::nullopt;
    }
    default:
      return std::nullopt;
  }
}

namespace detail {

// Counts how many items precede the 0xff break in an indefinite-length
// array, without allocating anything -- the pre-pass that lets the real
// decode below reserve the right-sized contiguous slot before decoding
// any child, instead of after.
template <std::size_t MaxDepth>
constexpr std::optional<std::uint64_t> count_indefinite_array_items(std::span<const std::uint8_t> body) {
  std::uint64_t count = 0;
  std::size_t consumed = 0;
  while (true) {
    if (consumed >= body.size()) return std::nullopt;
    if (body[consumed] == 0xff) return count;
    const auto len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!len) return std::nullopt;
    consumed += *len;
    ++count;
  }
}

// Same idea, but walks key+value pairs.
template <std::size_t MaxDepth>
constexpr std::optional<std::uint64_t> count_indefinite_map_pairs(std::span<const std::uint8_t> body) {
  std::uint64_t count = 0;
  std::size_t consumed = 0;
  while (true) {
    if (consumed >= body.size()) return std::nullopt;
    if (body[consumed] == 0xff) return count;
    const auto key_len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!key_len) return std::nullopt;
    consumed += *key_len;
    const auto value_len = skip_item<MaxDepth>(body.subspan(consumed));
    if (!value_len) return std::nullopt;
    consumed += *value_len;
    ++count;
  }
}

// A CBOR length/count field is a uint64_t (the wire format allows up to
// 2^64-1 items), but the reference-slot array indexes with std::size_t,
// which can be narrower (32 bits on common embedded targets).
// Arena::decode_value reserves a reference slot for every item *before*
// decoding any of them (see the array/map cases below), so a silently-
// truncating cast here would be exactly the libcbor PR #387 shape of bug:
// a huge count wraps to a small one, reserve_refs "succeeds" against that
// undersized number, and the loop -- which still iterates the real,
// untruncated uint64_t count -- then writes past the reservation.
// `multiplier` additionally guards the *2 the map case needs for its
// key+value slot pairs. Returns nullopt (the same refusal every other
// contract violation in this codec uses) instead of truncating.
constexpr std::optional<std::size_t> checked_node_count(std::uint64_t count, std::size_t multiplier = 1) {
  const std::uint64_t size_max = std::numeric_limits<std::size_t>::max();
  const std::uint64_t wide_multiplier = multiplier;
  if (multiplier > 1 && count > size_max / wide_multiplier) return std::nullopt;
  if (count > size_max) return std::nullopt;
  // ConstexprCbor::detail::narrow, not a plain copy-init: size_t and
  // uint64_t are the identical type on a 64-bit target (where an explicit
  // static_cast between them is a literal no-op GCC's -Wuseless-cast
  // flags), but genuinely different widths on a 32-bit one (wasm32 via
  // Emscripten, or a 32-bit embedded target), where this is exactly the
  // narrowing this function exists to guard, not hide -- narrow<>() picks
  // whichever of those two is actually true for the target at hand.
  return ConstexprCbor::detail::narrow<std::size_t>(count * wide_multiplier);
}

}  // namespace detail

// Structural equality between two already-decoded nodes, by pointer --
// needed to detect a duplicate map key, since an arena map has no flat,
// directly-comparable entries container the way the heap-based decoder's
// std::vector<pair<CborValue,CborValue>> does. Mirrors CborValue's own
// generated operator== in both scope (every alternative, including
// Array/Map/Tag keys, which are legal CBOR map keys even if unusual) and
// semantics (order-sensitive for arrays/maps, no canonical-equivalence
// resolution -- bytes are bytes, same stance as the rest of this library).
constexpr bool node_equal(const NodePool& pool, const Node* a_ptr, const Node* b_ptr) {
  const Node& a = *a_ptr;
  const Node& b = *b_ptr;
  return std::visit(
      [&](const auto& va) {
        return std::visit(
            [&](const auto& vb) -> bool {
              using TA = std::decay_t<decltype(va)>;
              using TB = std::decay_t<decltype(vb)>;
              if constexpr (!std::is_same_v<TA, TB>) {
                return false;
              } else if constexpr (std::is_same_v<TA, StringRef>) {
                const auto ca = pool.bytes_of(va);
                const auto cb = pool.bytes_of(vb);
                return va.kind == vb.kind && std::equal(ca.begin(), ca.end(), cb.begin(), cb.end());
              } else if constexpr (std::is_same_v<TA, BigNumRef>) {
                const auto ca = pool.bytes_of(va);
                const auto cb = pool.bytes_of(vb);
                return va.negative == vb.negative && std::equal(ca.begin(), ca.end(), cb.begin(), cb.end());
              } else if constexpr (std::is_same_v<TA, ArrayRef>) {
                if (va.count != vb.count) return false;
                for (std::size_t i = 0; i < va.count; ++i) {
                  if (!node_equal(pool, va.first_child[i], vb.first_child[i])) return false;
                }
                return true;
              } else if constexpr (std::is_same_v<TA, MapRef>) {
                if (va.count != vb.count) return false;
                for (std::size_t i = 0; i < va.count; ++i) {
                  if (!node_equal(pool, va.first_pair[2 * i], vb.first_pair[2 * i]) ||
                      !node_equal(pool, va.first_pair[2 * i + 1], vb.first_pair[2 * i + 1])) {
                    return false;
                  }
                }
                return true;
              } else if constexpr (std::is_same_v<TA, TagRef>) {
                return va.number == vb.number && node_equal(pool, va.child, vb.child);
              } else {
                return va == vb;  // uint64_t, NegativeInt, double, bool, nullptr_t, monostate, SimpleValue
              }
            },
            b.data);
      },
      a.data);
}

// Arena-shaped counterpart of ::ConstexprCbor::is_duplicate_key_policy_v:
// checks the on_duplicate(NodePool&, Node**, size_t, Node*, Node*) member
// each policy in the enclosing namespace also provides. Once NodePool
// carries no NTTPs, this overload no longer needs to be a member
// *template* either -- but that also means a bare `&P::on_duplicate` is
// now ambiguous between it and the heap-shaped std::vector overload (both
// are plain, non-template overloads of the same name), so this needs the
// same static_cast-to-a-concrete-function-pointer-type pin
// ::ConstexprCbor::is_duplicate_key_policy_v uses for exactly that reason
// -- see that trait's own comment for why a plain std::is_invocable_r_v
// on a bare decltype(&P::on_duplicate) won't compile, and why the void_t
// partial specialization (not a direct bool_constant) is required: for a
// P that provides no Arena-shaped overload at all, the static_cast itself
// is a hard "no matching function" error, and a class template's
// base-class list isn't a SFINAE context, so void_t's own substitution-
// failure-is-not-an-error protection is what lets this trait still
// evaluate to false instead of aborting compilation.
template <typename P, typename = void>
struct is_duplicate_key_policy : std::false_type {};

template <typename P>
struct is_duplicate_key_policy<
    P, std::void_t<decltype(static_cast<bool (*)(NodePool&, Node**, std::size_t, Node*, Node*)>(&P::on_duplicate))>>
    : std::true_type {};

template <typename P>
inline constexpr bool is_duplicate_key_policy_v = is_duplicate_key_policy<P>::value;

struct DecodedNode {
  Node* node;
  std::size_t bytes_consumed;
};

// The arena counterpart of ConstexprCbor::decode_value: same contract
// (pure function over an immutable span, std::nullopt on any refusal),
// but every allocation goes into the caller-supplied `pool` instead of the
// heap -- running out of pool capacity is just another refusal,
// indistinguishable from a malformed-input one, exactly like
// NodePool::allocate_node/reserve_refs/allocate_bytes already do.
template <std::size_t MaxDepth = 32, typename Utf8Policy = NoUtf8Validation, typename KeyPolicy = RejectDuplicateKeys>
constexpr std::optional<DecodedNode> decode_value(NodePool& pool, std::span<const std::uint8_t> bytes,
                                                    std::size_t depth = 0) {
  if (bytes.empty()) return std::nullopt;
  const std::uint8_t major_type = bytes[0] >> 5;

  switch (major_type) {
    case 0: {
      const auto u = decode_uint(bytes);
      if (!u) return std::nullopt;
      const auto node = pool.allocate_node(Node{.data = u->value});
      if (!node) return std::nullopt;
      return DecodedNode{.node = node, .bytes_consumed = u->bytes_consumed};
    }
    case 1: {
      const auto n = decode_negint(bytes);
      if (!n) return std::nullopt;
      const auto node = pool.allocate_node(Node{.data = n->value});
      if (!node) return std::nullopt;
      return DecodedNode{.node = node, .bytes_consumed = n->bytes_consumed};
    }
    case 2:
    case 3: {
      const auto kind = (major_type == 2) ? CborValue::StringKind::Bytes : CborValue::StringKind::Text;
      if (is_indefinite_or_break(bytes)) {
        const std::uint8_t* start = pool.bytes_begin_ + pool.byte_count_;
        std::size_t consumed = 1;
        while (true) {
          if (consumed >= bytes.size()) return std::nullopt;
          if (bytes[consumed] == 0xff) { ++consumed; break; }
          const auto chunk = (major_type == 2) ? decode_byte_string(bytes.subspan(consumed))
                                                : decode_text_string<Utf8Policy>(bytes.subspan(consumed));
          if (!chunk) return std::nullopt;
          if (!pool.allocate_bytes(chunk->bytes)) return std::nullopt;
          consumed += chunk->bytes_consumed;
        }
        const std::size_t length = static_cast<std::size_t>((pool.bytes_begin_ + pool.byte_count_) - start);
        const auto node =
            pool.allocate_node(Node{.data = StringRef{.kind = kind, .data = start, .length = length}});
        if (!node) return std::nullopt;
        return DecodedNode{.node = node, .bytes_consumed = consumed};
      }
      const auto result = (major_type == 2) ? decode_byte_string(bytes) : decode_text_string<Utf8Policy>(bytes);
      if (!result) return std::nullopt;
      const auto data = pool.allocate_bytes(result->bytes);
      if (!data) return std::nullopt;
      const auto node =
          pool.allocate_node(Node{.data = StringRef{.kind = kind, .data = data, .length = result->bytes.size()}});
      if (!node) return std::nullopt;
      return DecodedNode{.node = node, .bytes_consumed = result->bytes_consumed};
    }
    case 4: {
      if (depth >= MaxDepth) return std::nullopt;
      std::size_t consumed = 1;
      std::uint64_t count;
      if (is_indefinite_or_break(bytes)) {
        const auto maybe_count = detail::count_indefinite_array_items<MaxDepth>(bytes.subspan(1));
        if (!maybe_count) return std::nullopt;
        count = *maybe_count;
      } else {
        const auto len = decode_length(bytes);
        if (!len) return std::nullopt;
        consumed = len->bytes_consumed;
        count = len->value;
      }
      const auto safe_count = detail::checked_node_count(count);
      if (!safe_count) return std::nullopt;
      const auto first_slot = pool.reserve_refs(*safe_count);
      if (!first_slot) return std::nullopt;
      // size_t loop counter, not uint64_t: count == *safe_count numerically
      // (checked_node_count already proved count fits in size_t), so
      // iterating directly in size_t avoids ever narrowing a uint64_t loop
      // index into item_index -- the narrowing itself, not just the value,
      // is what a 32-bit size_t target (e.g. wasm32 via Emscripten) flags,
      // even though the value was always in range.
      for (std::size_t i = 0; i < *safe_count; ++i) {
        if (consumed > bytes.size()) return std::nullopt;
        const auto item = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!item) return std::nullopt;
        first_slot[i] = item->node;
        consumed += item->bytes_consumed;
      }
      if (is_indefinite_or_break(bytes)) {
        if (consumed >= bytes.size() || bytes[consumed] != 0xff) return std::nullopt;
        ++consumed;
      }
      const auto node = pool.allocate_node(Node{.data = ArrayRef{.first_child = first_slot, .count = *safe_count}});
      if (!node) return std::nullopt;
      return DecodedNode{.node = node, .bytes_consumed = consumed};
    }
    case 5: {
      if (depth >= MaxDepth) return std::nullopt;
      static_assert(is_duplicate_key_policy_v<KeyPolicy>,
                    "KeyPolicy must provide static bool on_duplicate(NodePool&, Node** first_pair, "
                    "size_t existing_pair_index, Node* new_key, Node* new_value)");
      std::size_t consumed = 1;
      std::uint64_t pair_count;
      if (is_indefinite_or_break(bytes)) {
        const auto maybe_count = detail::count_indefinite_map_pairs<MaxDepth>(bytes.subspan(1));
        if (!maybe_count) return std::nullopt;
        pair_count = *maybe_count;
      } else {
        const auto len = decode_length(bytes);
        if (!len) return std::nullopt;
        consumed = len->bytes_consumed;
        pair_count = len->value;
      }
      // Upper-bound reservation: every parsed pair reserves a slot, but a
      // pair a policy drops (KeepFirstDuplicateKey) or merges into an
      // earlier one (KeepLastDuplicateKey) never advances `written`, so
      // some reserved slots at the tail may go unused -- harmless, since
      // MapRef.count == written and map_key/map_value only ever index
      // [0, written). checked_node_count(pair_count, 2) guards both the
      // *2 and the uint64_t->size_t narrowing in one place (see its own
      // comment for why: a silently-truncating cast here would reserve an
      // undersized block for a count the loop below still iterates in
      // full, the same shape as libcbor PR #387).
      const auto safe_pair_count = detail::checked_node_count(pair_count, 2);
      if (!safe_pair_count) return std::nullopt;
      const auto first_pair = pool.reserve_refs(*safe_pair_count);
      if (!first_pair) return std::nullopt;
      std::size_t written = 0;
      for (std::uint64_t i = 0; i < pair_count; ++i) {
        if (consumed > bytes.size()) return std::nullopt;
        const auto key = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!key) return std::nullopt;
        consumed += key->bytes_consumed;
        if (consumed > bytes.size()) return std::nullopt;
        const auto value = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(consumed), depth + 1);
        if (!value) return std::nullopt;
        consumed += value->bytes_consumed;

        std::optional<std::size_t> duplicate_at;
        for (std::size_t j = 0; j < written; ++j) {
          if (node_equal(pool, first_pair[2 * j], key->node)) {
            duplicate_at = j;
            break;
          }
        }
        if (duplicate_at) {
          if (!KeyPolicy::on_duplicate(pool, first_pair, *duplicate_at, key->node, value->node)) {
            return std::nullopt;
          }
        } else {
          first_pair[2 * written] = key->node;
          first_pair[2 * written + 1] = value->node;
          ++written;
        }
      }
      if (is_indefinite_or_break(bytes)) {
        if (consumed >= bytes.size() || bytes[consumed] != 0xff) return std::nullopt;
        ++consumed;
      }
      const auto node = pool.allocate_node(Node{.data = MapRef{.first_pair = first_pair, .count = written}});
      if (!node) return std::nullopt;
      return DecodedNode{.node = node, .bytes_consumed = consumed};
    }
    case 6: {
      if (depth >= MaxDepth) return std::nullopt;
      const auto len = decode_length(bytes);
      if (!len) return std::nullopt;
      const auto nested = decode_value<MaxDepth, Utf8Policy, KeyPolicy>(pool, bytes.subspan(len->bytes_consumed), depth + 1);
      if (!nested) return std::nullopt;
      if (len->value == 2 || len->value == 3) {
        if (const auto* string_ref = std::get_if<StringRef>(&nested->node->data);
            string_ref && string_ref->kind == CborValue::StringKind::Bytes) {
          const auto node = pool.allocate_node(Node{.data = BigNumRef{.negative = (len->value == 3),
                                                                       .data = string_ref->data,
                                                                       .length = string_ref->length}});
          if (!node) return std::nullopt;
          return DecodedNode{.node = node, .bytes_consumed = len->bytes_consumed + nested->bytes_consumed};
        }
      }
      const auto node = pool.allocate_node(Node{.data = TagRef{.number = len->value, .child = nested->node}});
      if (!node) return std::nullopt;
      return DecodedNode{.node = node, .bytes_consumed = len->bytes_consumed + nested->bytes_consumed};
    }
    case 7: {
      const auto s = decode_simple(bytes);
      if (!s) return std::nullopt;
      using K = DecodedSimple::Kind;
      Node* node = nullptr;
      switch (s->kind) {
        case K::False:
          node = pool.allocate_node(Node{.data = false});
          break;
        case K::True:
          node = pool.allocate_node(Node{.data = true});
          break;
        case K::Null:
          node = pool.allocate_node(Node{.data = nullptr});
          break;
        case K::Undefined:
          node = pool.allocate_node(Node{.data = std::monostate{}});
          break;
        case K::Float:
          node = pool.allocate_node(Node{.data = s->as_double});
          break;
        case K::Simple:
          node = pool.allocate_node(Node{.data = CborValue::SimpleValue{.number = s->simple_value}});
          break;
      }
      if (!node) return std::nullopt;
      return DecodedNode{.node = node, .bytes_consumed = s->bytes_consumed};
    }
    default:
      return std::nullopt;
  }
}

namespace detail {

constexpr bool append_header_into(std::span<std::uint8_t> out, std::size_t& pos, std::uint8_t major_type,
                                   std::uint64_t value) {
  const std::uint8_t mt = static_cast<std::uint8_t>(major_type << 5);
  std::size_t needed;
  if (value <= 23) {
    needed = 1;
  } else if (value <= 0xff) {
    needed = 2;
  } else if (value <= 0xffff) {
    needed = 3;
  } else if (value <= 0xffffffffull) {
    needed = 5;
  } else {
    needed = 9;
  }
  if (out.size() < pos + needed) return false;
  if (value <= 23) {
    out[pos++] = static_cast<std::uint8_t>(mt | value);
  } else if (value <= 0xff) {
    out[pos++] = static_cast<std::uint8_t>(mt | 24);
    out[pos++] = static_cast<std::uint8_t>(value);
  } else if (value <= 0xffff) {
    out[pos++] = static_cast<std::uint8_t>(mt | 25);
    out[pos++] = static_cast<std::uint8_t>(value >> 8);
    out[pos++] = static_cast<std::uint8_t>(value);
  } else if (value <= 0xffffffffull) {
    out[pos++] = static_cast<std::uint8_t>(mt | 26);
    for (int shift = 24; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(value >> shift);
  } else {
    out[pos++] = static_cast<std::uint8_t>(mt | 27);
    for (int shift = 56; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(value >> shift);
  }
  return true;
}

constexpr bool append_bytes_into(std::span<std::uint8_t> out, std::size_t& pos, std::span<const std::uint8_t> src) {
  if (out.size() < pos + src.size()) return false;
  for (const auto b : src) out[pos++] = b;
  return true;
}

// Mirrors ConstexprCbor::encode_float, but reuses the arena's own
// detail-free primitives instead of duplicating the half/single-float
// derivation -- half/single candidate search doesn't allocate, so it is
// shared as-is via the free functions in the enclosing ::ConstexprCbor
// namespace (try_encode_half_float, try_encode_single_float).
constexpr bool append_float_into(std::span<std::uint8_t> out, std::size_t& pos, double value) {
  if (const auto h = ConstexprCbor::detail::try_encode_half_float(value)) {
    if (out.size() < pos + 3) return false;
    out[pos++] = 0xf9;
    out[pos++] = static_cast<std::uint8_t>(*h >> 8);
    out[pos++] = static_cast<std::uint8_t>(*h);
    return true;
  }
  if (const auto s = ConstexprCbor::detail::try_encode_single_float(value)) {
    if (out.size() < pos + 5) return false;
    out[pos++] = 0xfa;
    for (int shift = 24; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(*s >> shift);
    return true;
  }
  if (out.size() < pos + 9) return false;
  const std::uint64_t d =
      std::isnan(value) ? 0x7ff8000000000000ull : std::bit_cast<std::uint64_t>(value);
  out[pos++] = 0xfb;
  for (int shift = 56; shift >= 0; shift -= 8) out[pos++] = static_cast<std::uint8_t>(d >> shift);
  return true;
}

}  // namespace detail

// The arena counterpart of ConstexprCbor::encode_value: writes into a
// caller-supplied fixed buffer and returns how many bytes it used, or
// nullopt if the value doesn't fit -- never allocates, same discipline as
// decode_value above.
constexpr std::optional<std::size_t> encode_value(const NodePool& pool, const Node* node, std::span<std::uint8_t> out,
                                                    std::size_t pos = 0) {
  return std::visit(
      [&](const auto& v) -> std::optional<std::size_t> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          if (out.size() < pos + 1) return std::nullopt;
          out[pos++] = 0xf7;
          return pos;
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
          if (out.size() < pos + 1) return std::nullopt;
          out[pos++] = 0xf6;
          return pos;
        } else if constexpr (std::is_same_v<T, bool>) {
          if (out.size() < pos + 1) return std::nullopt;
          out[pos++] = v ? std::uint8_t{0xf5} : std::uint8_t{0xf4};
          return pos;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          if (!detail::append_header_into(out, pos, 0, v)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, NegativeInt>) {
          if (!detail::append_header_into(out, pos, 1, v.n)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, double>) {
          if (!detail::append_float_into(out, pos, v)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, StringRef>) {
          const auto content = pool.bytes_of(v);
          const std::uint8_t mt = (v.kind == CborValue::StringKind::Bytes) ? 2 : 3;
          if (!detail::append_header_into(out, pos, mt, content.size())) return std::nullopt;
          if (!detail::append_bytes_into(out, pos, content)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, BigNumRef>) {
          const auto content = pool.bytes_of(v);
          if (!detail::append_header_into(out, pos, 6, v.negative ? std::uint64_t{3} : std::uint64_t{2}))
            return std::nullopt;
          if (!detail::append_header_into(out, pos, 2, content.size())) return std::nullopt;
          if (!detail::append_bytes_into(out, pos, content)) return std::nullopt;
          return pos;
        } else if constexpr (std::is_same_v<T, CborValue::SimpleValue>) {
          if (v.number <= 23) {
            if (out.size() < pos + 1) return std::nullopt;
            out[pos++] = static_cast<std::uint8_t>(0xe0 | v.number);
          } else {
            if (out.size() < pos + 2) return std::nullopt;
            out[pos++] = 0xf8;
            out[pos++] = v.number;
          }
          return pos;
        } else if constexpr (std::is_same_v<T, ArrayRef>) {
          if (!detail::append_header_into(out, pos, 4, v.count)) return std::nullopt;
          for (std::size_t i = 0; i < v.count; ++i) {
            const auto written = encode_value(pool, v.first_child[i], out, pos);
            if (!written) return std::nullopt;
            pos = *written;
          }
          return pos;
        } else if constexpr (std::is_same_v<T, MapRef>) {
          if (!detail::append_header_into(out, pos, 5, v.count)) return std::nullopt;
          for (std::size_t i = 0; i < v.count; ++i) {
            const auto key_written = encode_value(pool, v.first_pair[2 * i], out, pos);
            if (!key_written) return std::nullopt;
            pos = *key_written;
            const auto value_written = encode_value(pool, v.first_pair[2 * i + 1], out, pos);
            if (!value_written) return std::nullopt;
            pos = *value_written;
          }
          return pos;
        } else {  // TagRef
          if (!detail::append_header_into(out, pos, 6, v.number)) return std::nullopt;
          const auto written = encode_value(pool, v.child, out, pos);
          if (!written) return std::nullopt;
          return *written;
        }
      },
      node->data);
}

}  // namespace ConstexprCbor::Arena

namespace ConstexprCbor {

// Out-of-line definition, placed here (rather than in constexpr_cbor.hpp)
// because Arena::Node needs to be complete (see the forward declaration
// and the in-class declaration in constexpr_cbor.hpp, next to
// RejectDuplicateKeys and KeepFirstDuplicateKey's own Arena overloads). A
// project that includes only constexpr_cbor.hpp never instantiates this
// template, so it never needs this definition at all; a project that
// includes this file gets it.
constexpr bool KeepLastDuplicateKey::on_duplicate(Arena::NodePool&, Arena::Node** first_pair,
                                                   std::size_t existing_pair_index, Arena::Node* /*new_key*/,
                                                   Arena::Node* new_value) {
  // Overwrite the existing pair's value slot in place -- the pair stays
  // at its original position, only the reference it points to changes.
  first_pair[2 * existing_pair_index + 1] = new_value;
  return true;
}

}  // namespace ConstexprCbor
