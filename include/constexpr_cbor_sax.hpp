#pragma once

#include <ranges>

#include "constexpr_cbor.hpp"

// A third decode strategy alongside constexpr_cbor.hpp (heap tree) and
// constexpr_cbor_noalloc.hpp (no-malloc tree): no tree is built.
// SaxRange pulls one CBOR event at a time off the wire (BeginArray, Uint,
// TextString, EndMap, ...), like the pull APIs of tinycbor and QCBOR. It reuses
// constexpr_cbor.hpp's byte-level primitives unchanged (decode_length,
// decode_uint/negint, decode_byte_string/decode_text_string, decode_simple,
// skip_item); only the control flow that decides which to call next is new.
//
// The existing decoders recurse over whole subtrees, so there is no
// "decode one event and pause" shape to reuse. Coroutines were rejected because
// C++20 coroutines cannot be evaluated in a constant expression, which would
// defeat the library's consteval point. Instead SaxIterator keeps an explicit
// frame stack bounded by MaxDepth, mirroring the depth cap of the tree decoders.
//
// All iteration state (position, frame stack, key-tracking buffer) lives by
// value in the iterator, so copies advance independently. SaxRange is therefore
// a std::ranges::forward_range (not just input_range) and implements
// std::ranges::view_interface, composing with range adaptors.
namespace ConstexprCbor::Sax {

enum class EventKind {
  BeginArray,
  EndArray,
  BeginMap,
  EndMap,
  Uint,
  NegInt,
  Bool,
  Null,
  Undefined,
  Float,
  TextString,
  ByteString,
  Tag,
  Simple,  // an unassigned simple value (RFC 8949 Table 4) -- see simple_value
  Error,
};

// The role the item plays in its immediate parent, so a consumer can tell "this
// scalar is a map key" without a separate event kind per (kind, role) pair.
// A tag has no role of its own: Tag events and the events of the item they wrap
// inherit the tag's context, since a tag modifies the next item rather than
// holding a child slot.
enum class EventContext { TopLevel, ArrayElement, MapKey, MapValue };

// Reuses the decoded-payload shapes (DecodedUint, DecodedNegint, DecodedSimple,
// DecodedBytes); only the switch on kind is new.
struct Event {
  EventKind kind = EventKind::Error;
  EventContext context = EventContext::TopLevel;
  std::uint64_t uint_value = 0;        // Uint; Tag's tag number; BeginArray/BeginMap's item/pair count
  bool has_count = true;               // BeginArray/BeginMap only: false means indefinite-length
  NegativeInt negint_value{};          // NegInt
  bool bool_value = false;             // Bool
  double float_value = 0.0;            // Float
  std::span<const std::uint8_t> bytes{};  // TextString/ByteString content -- zero-copy, same as decode_byte_string
  std::uint8_t simple_value = 0;       // Simple
  Error error{};                       // Error only -- the same reason+offset type the heap/NoAlloc
                                        // decoders report, so all three paths share one error vocabulary
};

namespace detail {

// Duplicate-key tracking is a flat, caller-sized array of (offset, length) spans
// into the input, shared by all open maps (each map's slice is
// [tracked_keys_begin, tracked_keys_begin + tracked_keys_count)).
// Keys are compared by raw encoded bytes, not by value as node_equal and
// CborValue::operator== do, so non-canonical encodings of one integer key
// (0x00 vs 0x18 0x00) are not caught here, unlike in the other decoders.
// Comparing by value would require decoding both keys, which builds the kind of
// tree this decoder exists to avoid.
struct KeySpan {
  std::size_t offset = 0;
  std::size_t length = 0;
};

struct Frame {
  enum class Kind : std::uint8_t { Array, Map };
  Kind kind = Kind::Array;
  bool indefinite = false;
  std::uint64_t remaining = 0;   // items (Array) or pairs not yet STARTED (Map)
  bool awaiting_value = false;   // Map only: true once this pair's key has been emitted
  std::size_t tag_depth_owed = 0;  // nesting budget to release (beyond this frame's own 1) when it closes
  EventContext role_in_parent = EventContext::TopLevel;
  std::size_t tracked_keys_begin = 0;
  std::size_t tracked_keys_count = 0;
};

}  // namespace detail

// MaxDepth bounds total nesting (array, map and tag combined), as in the other
// decoders. MaxTrackedKeys bounds the map keys remembered across all
// simultaneously open maps, for RejectDuplicateKeys/KeepFirstDuplicateKey.
// With 0 (fine alongside NoDuplicateKeyCheck, which tracks nothing), those two
// policies fail as soon as a map is entered.
template <std::size_t MaxDepth = 32, std::size_t MaxTrackedKeys = 0, typename Utf8Policy = NoUtf8Validation,
          typename KeyPolicy = NoDuplicateKeyCheck>
struct SaxRange : std::ranges::view_interface<SaxRange<MaxDepth, MaxTrackedKeys, Utf8Policy, KeyPolicy>> {
  static_assert(is_utf8_validation_policy_v<Utf8Policy>,
                "Utf8Policy must provide static bool is_valid(std::span<const std::uint8_t>)");
  static_assert(is_sax_duplicate_key_policy_v<KeyPolicy>,
                "KeyPolicy must provide static bool on_duplicate() for SAX decoding -- "
                "KeepLastDuplicateKey is not supported here (it would require buffering an "
                "already-emitted event to retroactively overwrite it)");

  std::span<const std::uint8_t> bytes{};

  constexpr SaxRange() = default;
  constexpr explicit SaxRange(std::span<const std::uint8_t> b) : bytes(b) {}

  struct Sentinel {};

  struct Iterator {
    using value_type = Event;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    std::span<const std::uint8_t> bytes{};
    std::size_t pos = 0;
    std::array<detail::Frame, MaxDepth> stack{};
    std::size_t depth = 0;
    std::size_t nesting_budget_used = 0;
    std::size_t pending_tag_depth = 0;
    bool finished = false;  // the top-level item's closing/only event has been produced
    bool at_end = false;    // iteration is actually over (compares equal to Sentinel)
    bool errored = false;
    Event current{};

    std::array<detail::KeySpan, (MaxTrackedKeys == 0 ? 1 : MaxTrackedKeys)> tracked_keys{};
    std::size_t tracked_keys_used = 0;

    static constexpr bool dedup_enabled = MaxTrackedKeys > 0 && !std::is_same_v<KeyPolicy, NoDuplicateKeyCheck>;

    constexpr Iterator() = default;

    constexpr const Event& operator*() const { return current; }

    constexpr Iterator& operator++() {
      advance();
      return *this;
    }
    constexpr Iterator operator++(int) {
      Iterator tmp = *this;
      advance();
      return tmp;
    }

    constexpr bool operator==(const Iterator& other) const {
      return pos == other.pos && depth == other.depth && at_end == other.at_end && errored == other.errored;
    }
    // Not `|| errored`: an error is delivered as one visible Error event first
    // (the iterator stays != end() for it), and only the next advance() sets
    // at_end. A range-for checks the sentinel before dereferencing, so folding
    // `errored` in here would stop the loop without the consumer seeing why.
    constexpr bool operator==(Sentinel) const { return at_end; }

   private:
    constexpr void set_error(ErrorReason reason, std::size_t offset) {
      errored = true;
      current = Event{.kind = EventKind::Error, .error = Error{.reason = reason, .offset = offset}};
    }

    // Returns true if `key_span`'s bytes match one of the keys already
    // tracked for `frame`.
    constexpr bool is_tracked_duplicate(const detail::Frame& frame, std::span<const std::uint8_t> key_span) const {
      for (std::size_t i = 0; i < frame.tracked_keys_count; ++i) {
        const auto& k = tracked_keys[frame.tracked_keys_begin + i];
        const auto existing = bytes.subspan(k.offset, k.length);
        if (existing.size() == key_span.size() && std::equal(existing.begin(), existing.end(), key_span.begin())) {
          return true;
        }
      }
      return false;
    }

    // Pops the now-exhausted top frame, emits its End event into `current`,
    // and updates whatever is now the new top (or finishes the range).
    constexpr void pop_and_emit_end() {
      detail::Frame& top = stack[depth - 1];
      if (top.indefinite) ++pos;  // consume the 0xff break byte
      const EventContext closing_role = top.role_in_parent;
      const EventKind end_kind = (top.kind == detail::Frame::Kind::Array) ? EventKind::EndArray : EventKind::EndMap;
      current = Event{.kind = end_kind, .context = closing_role};
      nesting_budget_used -= (1 + top.tag_depth_owed);
      --depth;
      if (depth > 0) {
        detail::Frame& parent = stack[depth - 1];
        if (closing_role == EventContext::MapKey) {
          parent.awaiting_value = true;
        } else {  // ArrayElement or MapValue
          if (closing_role == EventContext::MapValue) parent.awaiting_value = false;
          if (!parent.indefinite) --parent.remaining;
        }
      } else {
        finished = true;
      }
    }

    // Decodes exactly one event's worth of input at `pos` for role `ctx`
    // (a tag, a scalar, or a container-begin) and writes it into `current`.
    constexpr void decode_one_item(EventContext ctx) {
      if (pos >= bytes.size()) {
        set_error(ErrorReason::TruncatedInput, pos);
        return;
      }
      const auto rest = bytes.subspan(pos);
      const std::uint8_t major_type = rest[0] >> 5;
      if (major_type == 6) {
        if (nesting_budget_used >= MaxDepth) {
          set_error(ErrorReason::MaxDepthExceeded, pos);
          return;
        }
        const auto len = decode_length(rest);
        if (!len) {
          set_error(len.error().reason, pos + len.error().offset);
          return;
        }
        ++pending_tag_depth;
        ++nesting_budget_used;
        pos += len->bytes_consumed;
        current = Event{.kind = EventKind::Tag, .context = ctx, .uint_value = len->value};
        return;
      }
      switch (major_type) {
        case 0: {
          const auto u = decode_uint(rest);
          if (!u) {
            set_error(u.error().reason, pos + u.error().offset);
            return;
          }
          pos += u->bytes_consumed;
          release_pending_tag_depth();
          current = Event{.kind = EventKind::Uint, .context = ctx, .uint_value = u->value};
          after_leaf(ctx);
          return;
        }
        case 1: {
          const auto n = decode_negint(rest);
          if (!n) {
            set_error(n.error().reason, pos + n.error().offset);
            return;
          }
          pos += n->bytes_consumed;
          release_pending_tag_depth();
          current = Event{.kind = EventKind::NegInt, .context = ctx, .negint_value = n->value};
          after_leaf(ctx);
          return;
        }
        case 2: {
          const auto b = decode_byte_string(rest);
          if (!b) {
            set_error(b.error().reason, pos + b.error().offset);
            return;
          }
          pos += b->bytes_consumed;
          release_pending_tag_depth();
          current = Event{.kind = EventKind::ByteString, .context = ctx, .bytes = b->bytes};
          after_leaf(ctx);
          return;
        }
        case 3: {
          const auto s = decode_text_string<Utf8Policy>(rest);
          if (!s) {
            set_error(s.error().reason, pos + s.error().offset);
            return;
          }
          pos += s->bytes_consumed;
          release_pending_tag_depth();
          current = Event{.kind = EventKind::TextString, .context = ctx, .bytes = s->bytes};
          after_leaf(ctx);
          return;
        }
        case 4:
        case 5: {
          if (nesting_budget_used >= MaxDepth || depth >= MaxDepth) {
            set_error(ErrorReason::MaxDepthExceeded, pos);
            return;
          }
          std::uint64_t count = 0;
          bool indefinite = false;
          if (is_indefinite_or_break(rest)) {
            indefinite = true;
            pos += 1;
          } else {
            const auto len = decode_length(rest);
            if (!len) {
              set_error(len.error().reason, pos + len.error().offset);
              return;
            }
            count = len->value;
            pos += len->bytes_consumed;
          }
          detail::Frame f{};
          f.kind = (major_type == 4) ? detail::Frame::Kind::Array : detail::Frame::Kind::Map;
          f.indefinite = indefinite;
          f.remaining = count;
          f.tag_depth_owed = pending_tag_depth;
          f.role_in_parent = ctx;
          pending_tag_depth = 0;
          if (dedup_enabled && f.kind == detail::Frame::Kind::Map) {
            f.tracked_keys_begin = tracked_keys_used;
          }
          stack[depth++] = f;
          ++nesting_budget_used;
          current = Event{.kind = (major_type == 4) ? EventKind::BeginArray : EventKind::BeginMap,
                           .context = ctx,
                           .uint_value = count,
                           .has_count = !indefinite};
          return;
        }
        case 7: {
          const auto s = decode_simple(rest);
          if (!s) {
            set_error(s.error().reason, pos + s.error().offset);
            return;
          }
          pos += s->bytes_consumed;
          release_pending_tag_depth();
          using K = DecodedSimple::Kind;
          switch (s->kind) {
            case K::False:
              current = Event{.kind = EventKind::Bool, .context = ctx, .bool_value = false};
              break;
            case K::True:
              current = Event{.kind = EventKind::Bool, .context = ctx, .bool_value = true};
              break;
            case K::Null:
              current = Event{.kind = EventKind::Null, .context = ctx};
              break;
            case K::Undefined:
              current = Event{.kind = EventKind::Undefined, .context = ctx};
              break;
            case K::Float:
              current = Event{.kind = EventKind::Float, .context = ctx, .float_value = s->as_double};
              break;
            case K::Simple:
              current = Event{.kind = EventKind::Simple, .context = ctx, .simple_value = s->simple_value};
              break;
          }
          after_leaf(ctx);
          return;
        }
        default:
          set_error(ErrorReason::MalformedHeader, pos);
          return;
      }
    }

    constexpr void release_pending_tag_depth() {
      nesting_budget_used -= pending_tag_depth;
      pending_tag_depth = 0;
    }

    // Updates the parent frame (or finishes the whole range, at depth 0)
    // after a scalar leaf was just decoded for role `ctx`.
    constexpr void after_leaf(EventContext ctx) {
      if (ctx == EventContext::TopLevel) {
        finished = true;
        return;
      }
      detail::Frame& parent = stack[depth - 1];
      if (ctx == EventContext::MapKey) {
        parent.awaiting_value = true;
      } else {  // ArrayElement or MapValue
        if (ctx == EventContext::MapValue) parent.awaiting_value = false;
        if (!parent.indefinite) --parent.remaining;
      }
    }

    // Called when about to start a map pair with dedup tracking enabled: skips
    // leading duplicate pairs (per KeyPolicy's verdict) and records the first
    // non-duplicate key's span. Returns true if a non-duplicate key is ready for
    // decode_one_item; false if the map was exhausted while skipping (the caller
    // re-checks frame exhaustion) or an error occurred (errored_ is set).
    constexpr bool skip_duplicate_pairs(detail::Frame& frame) {
      for (;;) {
        const auto key_len = skip_item<MaxDepth>(bytes.subspan(pos));
        if (!key_len) {
          set_error(key_len.error().reason, pos + key_len.error().offset);
          return false;
        }
        const auto key_span = bytes.subspan(pos, *key_len);
        if (!is_tracked_duplicate(frame, key_span)) {
          if (tracked_keys_used >= MaxTrackedKeys) {
            // capacity exhaustion is a clean refusal, same stance as NodePool's
            set_error(ErrorReason::KeyTrackingCapacityExhausted, pos);
            return false;
          }
          tracked_keys[tracked_keys_used] = detail::KeySpan{.offset = pos, .length = *key_len};
          ++tracked_keys_used;
          ++frame.tracked_keys_count;
          return true;
        }
        const auto value_len = skip_item<MaxDepth>(bytes.subspan(pos + *key_len));
        if (!value_len) {
          set_error(value_len.error().reason, pos + *key_len + value_len.error().offset);
          return false;
        }
        if (!KeyPolicy::on_duplicate()) {
          set_error(ErrorReason::DuplicateKey, pos);  // RejectDuplicateKeys
          return false;
        }
        pos += *key_len + *value_len;
        if (!frame.indefinite) --frame.remaining;
        const bool exhausted =
            frame.indefinite ? (pos < bytes.size() && bytes[pos] == 0xff) : (frame.remaining == 0);
        if (exhausted) return false;
      }
    }

   public:
    constexpr void advance() {
      if (errored) {
        // The Error event was already delivered; this advance() runs after the
        // consumer saw it, so now stop.
        at_end = true;
        return;
      }
      for (;;) {
        if (depth > 0) {
          detail::Frame& top = stack[depth - 1];
          if (!top.awaiting_value) {
            bool exhausted;
            if (top.indefinite) {
              if (pos >= bytes.size()) {
                set_error(ErrorReason::TruncatedInput, pos);
                return;
              }
              exhausted = bytes[pos] == 0xff;
            } else {
              exhausted = (top.remaining == 0);
            }
            if (exhausted) {
              pop_and_emit_end();
              return;
            }
          }
        } else if (finished) {
          at_end = true;
          return;
        }

        EventContext ctx;
        if (depth == 0) {
          ctx = EventContext::TopLevel;
        } else {
          detail::Frame& top = stack[depth - 1];
          if (top.kind == detail::Frame::Kind::Array) {
            ctx = EventContext::ArrayElement;
          } else if (top.awaiting_value) {
            ctx = EventContext::MapValue;
          } else {
            ctx = EventContext::MapKey;
            if (dedup_enabled) {
              if (!skip_duplicate_pairs(top)) {
                if (errored) return;
                continue;  // map exhausted while skipping -- let the top of the loop pop it
              }
            }
          }
        }
        decode_one_item(ctx);
        return;
      }
    }
  };

  constexpr Iterator begin() const {
    Iterator it{};
    it.bytes = bytes;
    it.advance();
    return it;
  }
  constexpr Sentinel end() const { return {}; }
};

}  // namespace ConstexprCbor::Sax
