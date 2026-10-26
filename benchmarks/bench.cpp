// Benchmarks constexpr-cbor (heap CborValue, and the no-malloc Arena)
// against tinycbor and QCBOR on the *same* encoded bytes (benchmarks/data,
// embedded via payloads.h). Every library's decode function does the same
// amount of work: fully walk the decoded structure and fold every scalar
// into a checksum. If two libraries disagree on a payload's checksum,
// that's a correctness signal worth knowing about, not just a timing
// data point -- see README.md's "cross-check" column.
#include <malloc.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "constexpr_cbor.hpp"
#include "constexpr_cbor_arena.hpp"
#include "payloads.h"

extern "C" {
#include <cbor.h>  // tinycbor
}
#include "qcbor/qcbor_decode.h"  // QCBOR

namespace {

struct Checksum {
  std::uint64_t items = 0;
  std::int64_t isum = 0;
  double fsum = 0.0;
  std::uint64_t strbytes = 0;

  friend bool operator==(const Checksum& a, const Checksum& b) {
    return a.items == b.items && a.isum == b.isum && a.strbytes == b.strbytes && std::fabs(a.fsum - b.fsum) < 1e-6;
  }
};

long long alloc_delta_begin() { return static_cast<long long>(mallinfo2().uordblks); }

// ---------------------------------------------------------------------
// constexpr-cbor, heap CborValue
// ---------------------------------------------------------------------

void walk_heap(const ConstexprCbor::CborValue& v, Checksum& sum) {
  using CV = ConstexprCbor::CborValue;
  sum.items++;
  std::visit(
      [&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::uint64_t>) {
          sum.isum += static_cast<std::int64_t>(x);
        } else if constexpr (std::is_same_v<T, ConstexprCbor::NegativeInt>) {
          sum.isum += x.to_int64().value_or(0);
        } else if constexpr (std::is_same_v<T, double>) {
          sum.fsum += x;
        } else if constexpr (std::is_same_v<T, CV::OwnedString>) {
          sum.strbytes += x.content.size();
        } else if constexpr (std::is_same_v<T, std::vector<CV>>) {
          for (const auto& e : x) walk_heap(e, sum);
        } else if constexpr (std::is_same_v<T, std::vector<std::pair<CV, CV>>>) {
          for (const auto& [k, val] : x) {
            walk_heap(k, sum);
            walk_heap(val, sum);
          }
        } else if constexpr (std::is_same_v<T, CV::Tag>) {
          walk_heap(x.value.front(), sum);
        }
      },
      v.data);
}

// Returns {success, checksum, net heap bytes allocated by the decode --
// measured while the decoded tree is still alive, i.e. the peak this
// decode actually needed, not the ~0 delta you'd see after it's freed}.
struct HeapResult {
  bool ok;
  Checksum sum;
  long long bytes_allocated;
};

HeapResult decode_heap(std::span<const std::uint8_t> bytes) {
  const long long before = alloc_delta_begin();
  const auto decoded = ConstexprCbor::decode_value(bytes);
  if (!decoded) return {false, {}, 0};
  Checksum sum;
  walk_heap(decoded->first, sum);
  const long long after = alloc_delta_begin();  // `decoded` is still alive here
  return {true, sum, after - before};
}

// ---------------------------------------------------------------------
// constexpr-cbor, no-malloc Arena
// ---------------------------------------------------------------------

void walk_arena_node(const ConstexprCbor::Arena::NodePool& pool, const ConstexprCbor::Arena::Node& node,
                      Checksum& sum) {
  using namespace ConstexprCbor::Arena;
  sum.items++;
  std::visit(
      [&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::uint64_t>) {
          sum.isum += static_cast<std::int64_t>(x);
        } else if constexpr (std::is_same_v<T, ConstexprCbor::NegativeInt>) {
          sum.isum += x.to_int64().value_or(0);
        } else if constexpr (std::is_same_v<T, double>) {
          sum.fsum += x;
        } else if constexpr (std::is_same_v<T, StringRef>) {
          sum.strbytes += pool.bytes_of(x).size();
        } else if constexpr (std::is_same_v<T, ArrayRef>) {
          for (std::size_t i = 0; i < x.count; ++i) walk_arena_node(pool, pool.child(x, i), sum);
        } else if constexpr (std::is_same_v<T, MapRef>) {
          for (std::size_t i = 0; i < x.count; ++i) {
            walk_arena_node(pool, pool.map_key(x, i), sum);
            walk_arena_node(pool, pool.map_value(x, i), sum);
          }
        } else if constexpr (std::is_same_v<T, TagRef>) {
          walk_arena_node(pool, pool.tag_child(x), sum);
        }
      },
      node.data);
}

// `pool` is provisioned once by the caller, outside the measured window
// below, exactly as a real embedded caller would provision a
// static/stack NodePool up front -- re-decoding into the same pool
// repeatedly (resetting its three cursors) is what the Arena API is
// actually for, so that's what's timed, not a fresh allocation per call.
HeapResult decode_arena(ConstexprCbor::Arena::NodePool& pool, std::span<const std::uint8_t> bytes) {
  pool.node_count_ = 0;
  pool.ref_count_ = 0;
  pool.byte_count_ = 0;
  const long long before = alloc_delta_begin();
  const auto decoded = ConstexprCbor::Arena::decode_value(pool, bytes);
  if (!decoded) return {false, {}, 0};
  Checksum sum;
  walk_arena_node(pool, *decoded->node, sum);
  const long long after = alloc_delta_begin();
  return {true, sum, after - before};
}

// ---------------------------------------------------------------------
// tinycbor
// ---------------------------------------------------------------------

void tinycbor_walk(CborValue* it, Checksum& sum) {
  while (!cbor_value_at_end(it)) {
    sum.items++;
    const CborType type = cbor_value_get_type(it);
    switch (type) {
      case CborIntegerType: {
        if (cbor_value_is_unsigned_integer(it)) {
          std::uint64_t v = 0;
          cbor_value_get_uint64(it, &v);
          sum.isum += static_cast<std::int64_t>(v);
        } else {
          std::int64_t v = 0;
          cbor_value_get_int64(it, &v);
          sum.isum += v;
        }
        cbor_value_advance_fixed(it);
        break;
      }
      case CborDoubleType: {
        double v = 0;
        cbor_value_get_double(it, &v);
        sum.fsum += v;
        cbor_value_advance_fixed(it);
        break;
      }
      case CborTextStringType:
      case CborByteStringType: {
        std::size_t len = 0;
        cbor_value_get_string_length(it, &len);
        sum.strbytes += len;
        cbor_value_advance(it);
        break;
      }
      case CborArrayType:
      case CborMapType: {
        CborValue recursed;
        cbor_value_enter_container(it, &recursed);
        tinycbor_walk(&recursed, sum);
        cbor_value_leave_container(it, &recursed);
        break;
      }
      default:
        cbor_value_advance(it);
        break;
    }
  }
}

HeapResult decode_tinycbor(std::span<const std::uint8_t> bytes) {
  CborParser parser;
  CborValue it;
  const long long before = alloc_delta_begin();
  if (cbor_parser_init(bytes.data(), bytes.size(), 0, &parser, &it) != CborNoError) {
    return {false, {}, 0};
  }
  Checksum sum;
  tinycbor_walk(&it, sum);
  const long long after = alloc_delta_begin();
  return {true, sum, after - before};
}

// ---------------------------------------------------------------------
// QCBOR
// ---------------------------------------------------------------------

// Returns the QCBORError as a positive int (0 == QCBOR_SUCCESS) in
// addition to the usual HeapResult, since *why* QCBOR refused a payload
// (e.g. its fixed QCBOR_MAX_ARRAY_NESTING = 15) is itself a result worth
// reporting, not just a pass/fail bit.
struct QcborResult {
  HeapResult r;
  int error;
};

QcborResult decode_qcbor(std::span<const std::uint8_t> bytes) {
  QCBORDecodeContext ctx;
  const long long before = alloc_delta_begin();
  const UsefulBufC buf{bytes.data(), bytes.size()};
  QCBORDecode_Init(&ctx, buf, QCBOR_DECODE_MODE_NORMAL);
  Checksum sum;
  QCBORItem item;
  for (;;) {
    const QCBORError err = QCBORDecode_GetNext(&ctx, &item);
    if (err == QCBOR_ERR_NO_MORE_ITEMS) break;
    if (err != QCBOR_SUCCESS) return {{false, {}, 0}, static_cast<int>(err)};
    sum.items++;
    // Map/array keys come back as this item's *label*, not as a separate
    // item the way heap/tinycbor/Arena's walkers see them -- folded in
    // here so the cross-library checksum is actually comparable.
    if (item.uLabelType == QCBOR_TYPE_TEXT_STRING || item.uLabelType == QCBOR_TYPE_BYTE_STRING) {
      sum.items++;
      sum.strbytes += item.label.string.len;
    }
    switch (item.uDataType) {
      case QCBOR_TYPE_INT64:
        sum.isum += item.val.int64;
        break;
      case QCBOR_TYPE_UINT64:
        sum.isum += static_cast<std::int64_t>(item.val.uint64);
        break;
      case QCBOR_TYPE_DOUBLE:
        sum.fsum += item.val.dfnum;
        break;
      case QCBOR_TYPE_TEXT_STRING:
      case QCBOR_TYPE_BYTE_STRING:
        sum.strbytes += item.val.string.len;
        break;
      default:
        break;
    }
  }
  const QCBORError finish_err = QCBORDecode_Finish(&ctx);
  if (finish_err != QCBOR_SUCCESS) return {{false, {}, 0}, static_cast<int>(finish_err)};
  const long long after = alloc_delta_begin();
  return {{true, sum, after - before}, 0};
}

// ---------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------

struct Payload {
  const char* name;
  const std::uint8_t* bytes;
  std::size_t len;
  int iterations;
};

template <typename F>
double time_ns_per_call(F&& f, int iterations) {
  // Warm up (page faults, branch predictor, etc.) before timing.
  for (int i = 0; i < 5; ++i) f();
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) f();
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(end - start).count() / iterations;
}

constexpr std::size_t kArenaMaxNodes = 65536;
constexpr std::size_t kArenaMaxRefs = 65536;
constexpr std::size_t kArenaMaxBytes = 262144;

void run_payload(const Payload& p) {
  const std::span<const std::uint8_t> bytes(p.bytes, p.len);
  static ConstexprCbor::Arena::ArenaStorage<kArenaMaxNodes, kArenaMaxRefs, kArenaMaxBytes> storage;
  static auto pool = storage.make_pool();

  const auto heap_r = decode_heap(bytes);
  const auto arena_r = decode_arena(pool, bytes);
  const auto tiny_r = decode_tinycbor(bytes);
  const auto qcbor_full = decode_qcbor(bytes);
  const auto& qcbor_r = qcbor_full.r;

  std::printf("\n== %s (%zu bytes) ==\n", p.name, p.len);
  std::printf("  decode ok:  heap=%d arena=%d tinycbor=%d qcbor=%d (qcbor error=%d)\n", heap_r.ok, arena_r.ok,
              tiny_r.ok, qcbor_r.ok, qcbor_full.error);
  std::printf("  checksum match: heap==arena=%d heap==tinycbor=%d heap==qcbor=%d\n", heap_r.sum == arena_r.sum,
              heap_r.sum == tiny_r.sum, heap_r.sum == qcbor_r.sum);
  std::printf("  items=%llu isum=%lld fsum=%.3f strbytes=%llu\n",
              static_cast<unsigned long long>(heap_r.sum.items), static_cast<long long>(heap_r.sum.isum),
              heap_r.sum.fsum, static_cast<unsigned long long>(heap_r.sum.strbytes));
  std::printf("  heap bytes allocated/decode:    %lld\n", heap_r.bytes_allocated);
  std::printf("  arena bytes allocated/decode:   %lld\n", arena_r.bytes_allocated);
  std::printf("  tinycbor bytes allocated/decode: %lld\n", tiny_r.bytes_allocated);
  std::printf("  qcbor bytes allocated/decode:    %lld\n", qcbor_r.bytes_allocated);

  const double heap_ns = time_ns_per_call([&] { (void)decode_heap(bytes); }, p.iterations);
  const double arena_ns = time_ns_per_call([&] { (void)decode_arena(pool, bytes); }, p.iterations);
  const double tiny_ns = time_ns_per_call([&] { (void)decode_tinycbor(bytes); }, p.iterations);
  const double qcbor_ns = time_ns_per_call([&] { (void)decode_qcbor(bytes); }, p.iterations);

  std::printf("  ns/decode: heap=%.0f arena=%.0f tinycbor=%.0f qcbor=%.0f\n", heap_ns, arena_ns, tiny_ns, qcbor_ns);
  std::printf("  MB/s:      heap=%.1f arena=%.1f tinycbor=%.1f qcbor=%.1f\n", p.len / (heap_ns * 1e-3),
              p.len / (arena_ns * 1e-3), p.len / (tiny_ns * 1e-3), p.len / (qcbor_ns * 1e-3));
}

}  // namespace

int main() {
  const Payload payloads[] = {
      {"small", small_bytes, small_len, 200000},
      {"medium", medium_bytes, medium_len, 5000},
      {"deep", deep_bytes, deep_len, 200000},
      {"strings", strings_bytes, strings_len, 20000},
  };
  for (const auto& p : payloads) run_payload(p);
  return 0;
}
