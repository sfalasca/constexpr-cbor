// Benchmarks constexpr-cbor (heap CborValue, NoAlloc, SAX) against tinycbor,
// QCBOR and libcbor on the same encoded bytes (benchmarks/data, via payloads.h).
// Every decode folds all scalars into a checksum; differing checksums between
// libraries are a correctness signal (README.md, "cross-check" column).
//
// libcbor is reached only through libcbor_adapter.hpp/.cpp: its header declares C
// functions with the same names as tinycbor's, so they cannot share a translation
// unit.
#include <malloc.h>
#include <sched.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "constexpr_cbor.hpp"
#include "constexpr_cbor_noalloc.hpp"
#include "constexpr_cbor_sax.hpp"
#include "libcbor_adapter.hpp"
#include "payloads.h"

extern "C" {
#include <cbor.h>  // tinycbor
}
#include "qcbor/qcbor_decode.h"  // QCBOR
#include "qcbor/qcbor_encode.h"  // QCBOR

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

// Encode-side result: whether it fit, bytes produced, and bytes allocated (nonzero
// only for libcbor, which must build a tree first).
struct EncodeResult {
  bool ok;
  std::size_t bytes_written;
  long long bytes_allocated;
};

long long alloc_delta_begin() { return static_cast<long long>(mallinfo2().uordblks); }

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

// Returns {success, checksum, net heap bytes allocated by the decode, measured
// while the decoded tree is still alive}.
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

// encode_value cannot run out of space (it grows a std::vector), so only its
// allocations are measured.
EncodeResult encode_heap(const ConstexprCbor::CborValue& value) {
  const long long before = alloc_delta_begin();
  const auto out = ConstexprCbor::encode_value(value);
  const long long after = alloc_delta_begin();
  return {true, out.size(), after - before};
}

void walk_noalloc_node(const ConstexprCbor::NoAlloc::NodePool& pool, const ConstexprCbor::NoAlloc::Node& node,
                      Checksum& sum) {
  using namespace ConstexprCbor::NoAlloc;
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
          for (std::size_t i = 0; i < x.count; ++i) walk_noalloc_node(pool, pool.child(x, i), sum);
        } else if constexpr (std::is_same_v<T, MapRef>) {
          for (std::size_t i = 0; i < x.count; ++i) {
            walk_noalloc_node(pool, pool.map_key(x, i), sum);
            walk_noalloc_node(pool, pool.map_value(x, i), sum);
          }
        } else if constexpr (std::is_same_v<T, TagRef>) {
          walk_noalloc_node(pool, pool.tag_child(x), sum);
        }
      },
      node.data);
}

// `pool` is provisioned once outside the timed window, as an embedded caller
// would; the timed work is re-decoding into it.
HeapResult decode_noalloc(ConstexprCbor::NoAlloc::NodePool& pool, std::span<const std::uint8_t> bytes) {
  pool.node_count_ = 0;
  pool.ref_count_ = 0;
  pool.byte_count_ = 0;
  const long long before = alloc_delta_begin();
  const auto decoded = ConstexprCbor::NoAlloc::decode_value(pool, bytes);
  if (!decoded) return {false, {}, 0};
  Checksum sum;
  walk_noalloc_node(pool, *decoded->node, sum);
  const long long after = alloc_delta_begin();
  return {true, sum, after - before};
}

// `node` must belong to `pool`: decode into it first (see run_payload).
EncodeResult encode_noalloc(const ConstexprCbor::NoAlloc::NodePool& pool, const ConstexprCbor::NoAlloc::Node* node,
                             std::span<std::uint8_t> out) {
  const long long before = alloc_delta_begin();
  const auto written = ConstexprCbor::NoAlloc::encode_value(pool, node, out);
  const long long after = alloc_delta_begin();
  if (!written) return {false, 0, 0};
  return {true, *written, after - before};
}

// SAX: no tree, one event at a time. The most comparable of the three
// constexpr-cbor paths to tinycbor/QCBOR, which are also pull parsers.

HeapResult decode_sax(std::span<const std::uint8_t> bytes) {
  using EK = ConstexprCbor::Sax::EventKind;
  const long long before = alloc_delta_begin();
  Checksum sum;
  bool ok = true;
  for (auto ev : ConstexprCbor::Sax::SaxRange<32>{bytes}) {
    switch (ev.kind) {
      case EK::Error:
        ok = false;
        break;
      case EK::EndArray:
      case EK::EndMap:
        break;  // the container's own item was already counted at BeginArray/BeginMap
      case EK::Uint:
        sum.items++;
        sum.isum += static_cast<std::int64_t>(ev.uint_value);
        break;
      case EK::NegInt:
        sum.items++;
        sum.isum += ev.negint_value.to_int64().value_or(0);
        break;
      case EK::Float:
        sum.items++;
        sum.fsum += ev.float_value;
        break;
      case EK::TextString:
      case EK::ByteString:
        sum.items++;
        sum.strbytes += ev.bytes.size();
        break;
      default:  // BeginArray, BeginMap, Tag, Bool, Null, Undefined, Simple
        sum.items++;
        break;
    }
  }
  const long long after = alloc_delta_begin();
  if (!ok) return {false, {}, 0};
  return {true, sum, after - before};
}

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

// tinycbor's encoder is not linked here: cborencoder.c and libcbor export
// identically-named C functions (cbor_encode_uint, cbor_encode_tag), a link-time
// collision that no translation-unit split avoids. It is measured by
// bench_tinycbor_encode.cpp instead (see benchmarks/README.md).

// Also returns the QCBORError (0 == QCBOR_SUCCESS), since why QCBOR refused a
// payload (e.g. QCBOR_MAX_ARRAY_NESTING = 15) is itself a result.
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
    // Map/array keys come back as the item's label; fold them in to keep checksums
    // comparable.
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

// QCBOR has no tree of its own, so this encodes straight from the shared decoded
// CborValue.
void qcbor_encode_value(const ConstexprCbor::CborValue& v, QCBOREncodeContext& enc) {
  using CV = ConstexprCbor::CborValue;
  std::visit(
      [&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          QCBOREncode_AddUndef(&enc);
        } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
          QCBOREncode_AddNULL(&enc);
        } else if constexpr (std::is_same_v<T, bool>) {
          QCBOREncode_AddBool(&enc, x);
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          QCBOREncode_AddUInt64(&enc, x);
        } else if constexpr (std::is_same_v<T, ConstexprCbor::NegativeInt>) {
          const auto i = x.to_int64();
          // QCBOR's basic int API cannot represent a magnitude past INT64_MIN; the
          // payloads never reach it (gen_payloads.py), so nothing is emitted.
          if (i) QCBOREncode_AddInt64(&enc, *i);
        } else if constexpr (std::is_same_v<T, CV::OwnedString>) {
          const UsefulBufC buf{x.content.data(), x.content.size()};
          if (x.kind == CV::StringKind::Text) {
            QCBOREncode_AddText(&enc, buf);
          } else {
            QCBOREncode_AddBytes(&enc, buf);
          }
        } else if constexpr (std::is_same_v<T, double>) {
          QCBOREncode_AddDouble(&enc, x);
        } else if constexpr (std::is_same_v<T, std::vector<CV>>) {
          QCBOREncode_OpenArray(&enc);
          for (const auto& e : x) qcbor_encode_value(e, enc);
          QCBOREncode_CloseArray(&enc);
        } else if constexpr (std::is_same_v<T, std::vector<std::pair<CV, CV>>>) {
          QCBOREncode_OpenMap(&enc);
          for (const auto& [k, val] : x) {
            qcbor_encode_value(k, enc);
            qcbor_encode_value(val, enc);
          }
          QCBOREncode_CloseMap(&enc);
        } else if constexpr (std::is_same_v<T, CV::Tag>) {
          QCBOREncode_AddTag(&enc, x.number);
          qcbor_encode_value(x.value.front(), enc);
        }
        // BigNum/SimpleValue: not exercised by this benchmark's payloads.
      },
      v.data);
}

EncodeResult encode_qcbor(const ConstexprCbor::CborValue& value, std::span<std::uint8_t> out) {
  QCBOREncodeContext enc;
  const UsefulBuf storage{out.data(), out.size()};
  QCBOREncode_Init(&enc, storage);
  qcbor_encode_value(value, enc);
  UsefulBufC encoded;
  const QCBORError err = QCBOREncode_Finish(&enc, &encoded);
  if (err != QCBOR_SUCCESS) return {false, 0, 0};
  return {true, encoded.len, 0};
}

// libcbor: DOM-style (cbor_load builds a ref-counted tree), like the heap
// CborValue path. decode_libcbor() lives in libcbor_adapter.cpp.

HeapResult decode_libcbor_checksum(std::span<const std::uint8_t> bytes) {
  const LibcborDecodeResult r = decode_libcbor(bytes);
  if (!r.ok) return {false, {}, 0};
  Checksum sum{r.items, r.isum, r.fsum, r.strbytes};
  return {true, sum, r.bytes_allocated};
}

// encode_libcbor builds libcbor's tree from the shared CborValue and serializes
// it. Tree construction counts as encode cost: libcbor has no streaming encoder.
EncodeResult encode_libcbor_checksum(const ConstexprCbor::CborValue& value, std::span<std::uint8_t> out) {
  const LibcborEncodeResult r = encode_libcbor(value, out);
  return {r.ok, r.bytes_written, r.bytes_allocated};
}

struct Payload {
  const char* name;
  const std::uint8_t* bytes;
  std::size_t len;
  int iterations;
};

// kRounds independent timed passes, reporting {min, max}: interference can only
// slow a pass, so the minimum estimates the undisturbed cost, and a wide spread
// flags a noisy run.
struct TimingStats {
  double min_ns;
  double max_ns;
};

constexpr int kRounds = 9;

template <typename F>
double time_one_round_ns_per_call(F&& f, int iterations) {
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) f();
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(end - start).count() / iterations;
}

constexpr std::size_t kNoAllocMaxNodes = 65536;
constexpr std::size_t kNoAllocMaxRefs = 65536;
constexpr std::size_t kNoAllocMaxBytes = 262144;

void run_payload(const Payload& p) {
  const std::span<const std::uint8_t> bytes(p.bytes, p.len);
  static ConstexprCbor::NoAlloc::NoAllocStorage<kNoAllocMaxNodes, kNoAllocMaxRefs, kNoAllocMaxBytes> storage;
  static auto pool = storage.make_pool();

  const auto heap_r = decode_heap(bytes);
  const auto noalloc_r = decode_noalloc(pool, bytes);
  const auto sax_r = decode_sax(bytes);
  const auto tiny_r = decode_tinycbor(bytes);
  const auto qcbor_full = decode_qcbor(bytes);
  const auto& qcbor_r = qcbor_full.r;
  const auto libcbor_r = decode_libcbor_checksum(bytes);

  std::printf("\n== %s (%zu bytes) ==\n", p.name, p.len);
  std::printf("  decode ok:  heap=%d noalloc=%d sax=%d tinycbor=%d qcbor=%d (qcbor error=%d) libcbor=%d\n", heap_r.ok,
              noalloc_r.ok, sax_r.ok, tiny_r.ok, qcbor_r.ok, qcbor_full.error, libcbor_r.ok);
  std::printf(
      "  checksum match: heap==noalloc=%d heap==sax=%d heap==tinycbor=%d heap==qcbor=%d heap==libcbor=%d\n",
      heap_r.sum == noalloc_r.sum, heap_r.sum == sax_r.sum, heap_r.sum == tiny_r.sum, heap_r.sum == qcbor_r.sum,
      heap_r.sum == libcbor_r.sum);
  std::printf("  items=%llu isum=%lld fsum=%.3f strbytes=%llu\n",
              static_cast<unsigned long long>(heap_r.sum.items), static_cast<long long>(heap_r.sum.isum),
              heap_r.sum.fsum, static_cast<unsigned long long>(heap_r.sum.strbytes));
  std::printf("  heap bytes allocated/decode:    %lld\n", heap_r.bytes_allocated);
  std::printf("  noalloc bytes allocated/decode:   %lld\n", noalloc_r.bytes_allocated);
  std::printf("  sax bytes allocated/decode:      %lld\n", sax_r.bytes_allocated);
  std::printf("  tinycbor bytes allocated/decode: %lld\n", tiny_r.bytes_allocated);
  std::printf("  qcbor bytes allocated/decode:    %lld\n", qcbor_r.bytes_allocated);
  std::printf("  libcbor bytes allocated/decode:  %lld\n", libcbor_r.bytes_allocated);

  // iterations/kRounds per round, so a disturbance spoils only one round.
  const int iters = std::max(200, p.iterations / kRounds);

  TimingStats heap_t{std::numeric_limits<double>::infinity(), 0.0};
  TimingStats noalloc_t = heap_t, sax_t = heap_t, tiny_t = heap_t, qcbor_t = heap_t, libcbor_t = heap_t;

  // Untimed warmup calls first (page faults, branch predictor, icache).
  for (int i = 0; i < 5; ++i) {
    (void)decode_heap(bytes);
    (void)decode_noalloc(pool, bytes);
    (void)decode_sax(bytes);
    (void)decode_tinycbor(bytes);
    (void)decode_qcbor(bytes);
    (void)decode_libcbor_checksum(bytes);
  }

  for (int round = 0; round < kRounds; ++round) {
    // Interleaved per round (not one library at a time), so a disturbance hits every
    // library roughly equally.
    const double h = time_one_round_ns_per_call([&] { (void)decode_heap(bytes); }, iters);
    heap_t.min_ns = std::min(heap_t.min_ns, h);
    heap_t.max_ns = std::max(heap_t.max_ns, h);
    const double a = time_one_round_ns_per_call([&] { (void)decode_noalloc(pool, bytes); }, iters);
    noalloc_t.min_ns = std::min(noalloc_t.min_ns, a);
    noalloc_t.max_ns = std::max(noalloc_t.max_ns, a);
    const double s = time_one_round_ns_per_call([&] { (void)decode_sax(bytes); }, iters);
    sax_t.min_ns = std::min(sax_t.min_ns, s);
    sax_t.max_ns = std::max(sax_t.max_ns, s);
    const double t = time_one_round_ns_per_call([&] { (void)decode_tinycbor(bytes); }, iters);
    tiny_t.min_ns = std::min(tiny_t.min_ns, t);
    tiny_t.max_ns = std::max(tiny_t.max_ns, t);
    const double q = time_one_round_ns_per_call([&] { (void)decode_qcbor(bytes); }, iters);
    qcbor_t.min_ns = std::min(qcbor_t.min_ns, q);
    qcbor_t.max_ns = std::max(qcbor_t.max_ns, q);
    const double l = time_one_round_ns_per_call([&] { (void)decode_libcbor_checksum(bytes); }, iters);
    libcbor_t.min_ns = std::min(libcbor_t.min_ns, l);
    libcbor_t.max_ns = std::max(libcbor_t.max_ns, l);
  }

  std::printf(
      "  ns/decode (min, over %d rounds of %d calls): heap=%.0f noalloc=%.0f sax=%.0f tinycbor=%.0f qcbor=%.0f "
      "libcbor=%.0f\n",
      kRounds, iters, heap_t.min_ns, noalloc_t.min_ns, sax_t.min_ns, tiny_t.min_ns, qcbor_t.min_ns, libcbor_t.min_ns);
  std::printf("  ns/decode (max, same rounds -- spread vs. min is this run's noise):"
              " heap=%.0f noalloc=%.0f sax=%.0f tinycbor=%.0f qcbor=%.0f libcbor=%.0f\n",
              heap_t.max_ns, noalloc_t.max_ns, sax_t.max_ns, tiny_t.max_ns, qcbor_t.max_ns, libcbor_t.max_ns);
  std::printf("  MB/s (from min): heap=%.1f noalloc=%.1f sax=%.1f tinycbor=%.1f qcbor=%.1f libcbor=%.1f\n",
              p.len / (heap_t.min_ns * 1e-3), p.len / (noalloc_t.min_ns * 1e-3), p.len / (sax_t.min_ns * 1e-3),
              p.len / (tiny_t.min_ns * 1e-3), p.len / (qcbor_t.min_ns * 1e-3), p.len / (libcbor_t.min_ns * 1e-3));

  // Encode: each library encodes the same already-decoded value, built outside the
  // timed region. QCBOR and libcbor build their native form from the shared heap
  // CborValue. tinycbor is measured separately (see its note above); SAX has no
  // encoder.
  pool.node_count_ = 0;
  pool.ref_count_ = 0;
  pool.byte_count_ = 0;
  const auto heap_decoded = ConstexprCbor::decode_value(bytes);
  const auto noalloc_decoded = ConstexprCbor::NoAlloc::decode_value(pool, bytes);
  if (!heap_decoded || !noalloc_decoded) {
    std::printf("  encode: skipped (decode for encode setup failed unexpectedly)\n");
    return;
  }
  const ConstexprCbor::CborValue& value = heap_decoded->first;
  const ConstexprCbor::NoAlloc::Node* noalloc_node = noalloc_decoded->node;

  static std::array<std::uint8_t, 1 << 20> out_buf;  // 1 MiB, comfortably larger than any payload here

  const auto heap_e = encode_heap(value);
  const auto noalloc_e = encode_noalloc(pool, noalloc_node, out_buf);
  const auto qcbor_e = encode_qcbor(value, out_buf);
  const auto libcbor_e = encode_libcbor_checksum(value, out_buf);

  std::printf("  encode ok:  heap=%d noalloc=%d qcbor=%d libcbor=%d\n", heap_e.ok, noalloc_e.ok, qcbor_e.ok,
              libcbor_e.ok);
  std::printf("  encoded bytes: heap=%zu noalloc=%zu qcbor=%zu libcbor=%zu (input was %zu; tinycbor measured "
              "separately, see bench_tinycbor_encode)\n",
              heap_e.bytes_written, noalloc_e.bytes_written, qcbor_e.bytes_written, libcbor_e.bytes_written, p.len);
  std::printf("  libcbor bytes allocated/encode (tree build -- no streaming encoder): %lld\n",
              libcbor_e.bytes_allocated);

  TimingStats heap_et{std::numeric_limits<double>::infinity(), 0.0};
  TimingStats noalloc_et = heap_et, qcbor_et = heap_et, libcbor_et = heap_et;

  for (int i = 0; i < 5; ++i) {
    (void)encode_heap(value);
    (void)encode_noalloc(pool, noalloc_node, out_buf);
    (void)encode_qcbor(value, out_buf);
    (void)encode_libcbor_checksum(value, out_buf);
  }

  for (int round = 0; round < kRounds; ++round) {
    const double h = time_one_round_ns_per_call([&] { (void)encode_heap(value); }, iters);
    heap_et.min_ns = std::min(heap_et.min_ns, h);
    heap_et.max_ns = std::max(heap_et.max_ns, h);
    const double a = time_one_round_ns_per_call([&] { (void)encode_noalloc(pool, noalloc_node, out_buf); }, iters);
    noalloc_et.min_ns = std::min(noalloc_et.min_ns, a);
    noalloc_et.max_ns = std::max(noalloc_et.max_ns, a);
    const double q = time_one_round_ns_per_call([&] { (void)encode_qcbor(value, out_buf); }, iters);
    qcbor_et.min_ns = std::min(qcbor_et.min_ns, q);
    qcbor_et.max_ns = std::max(qcbor_et.max_ns, q);
    const double l = time_one_round_ns_per_call([&] { (void)encode_libcbor_checksum(value, out_buf); }, iters);
    libcbor_et.min_ns = std::min(libcbor_et.min_ns, l);
    libcbor_et.max_ns = std::max(libcbor_et.max_ns, l);
  }

  std::printf("  ns/encode (min, over %d rounds of %d calls): heap=%.0f noalloc=%.0f qcbor=%.0f libcbor=%.0f\n",
              kRounds, iters, heap_et.min_ns, noalloc_et.min_ns, qcbor_et.min_ns, libcbor_et.min_ns);
  std::printf("  ns/encode (max, same rounds -- spread vs. min is this run's noise):"
              " heap=%.0f noalloc=%.0f qcbor=%.0f libcbor=%.0f\n",
              heap_et.max_ns, noalloc_et.max_ns, qcbor_et.max_ns, libcbor_et.max_ns);
  std::printf("  MB/s encode (from min, output bytes): heap=%.1f noalloc=%.1f qcbor=%.1f libcbor=%.1f\n",
              heap_e.bytes_written / (heap_et.min_ns * 1e-3), noalloc_e.bytes_written / (noalloc_et.min_ns * 1e-3),
              qcbor_e.bytes_written / (qcbor_et.min_ns * 1e-3), libcbor_e.bytes_written / (libcbor_et.min_ns * 1e-3));
}

}  // namespace

int main() {
  // Pin to one core so the scheduler cannot migrate the process mid-measurement.
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(0, &cpu_set);
  if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0) {
    std::printf("(note: sched_setaffinity failed, continuing unpinned -- timing noise may be higher)\n");
  }

  const Payload payloads[] = {
      {"small", small_bytes, small_len, 200000},
      {"medium", medium_bytes, medium_len, 5000},
      {"deep", deep_bytes, deep_len, 200000},
      {"strings", strings_bytes, strings_len, 20000},
  };
  for (const auto& p : payloads) run_payload(p);
  return 0;
}
