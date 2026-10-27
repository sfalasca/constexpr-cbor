#!/bin/sh
# Compiles every tests/*.cpp file expecting success, and every
# tests/expected_failures/*.cpp file expecting the opposite: a compile
# error, with a specific diagnostic substring where one is known. This is
# the no-CMake equivalent of what the desktop matrix's ctest run checks
# (positive tests build and run; expected_failures fail to compile with
# the right message) -- used by the embedded Dockerfiles, which have no
# CMake/ctest to do this for them.
#
# Also compiles every examples/*.cpp file, expecting success, the same
# no-CMake equivalent of what the desktop/wasm matrix's `cmake --build`
# step does automatically once examples/ became a CMake subdirectory --
# this is the embedded-targets' equivalent proof that the example files
# are real, working code against each cross-compiler, not just against
# the one compiler someone happened to write them with. Compile-only
# (-c, no link), same as the tests loop above and for the same reason:
# this is validating a header against a cross-compiler, not producing
# runnable firmware. Guarded on the directory existing so this script
# still works for any caller that doesn't copy examples/ in.
#
# Usage: compile-all-tests.sh <out-suffix> <compiler> <flag> [<flag> ...]
set -e

out_suffix="$1"
shift
compiler="$1"
shift

for f in tests/*.cpp; do
  echo "=== compiling (expect success) $f ==="
  "$compiler" "$@" -c "$f" -o "/tmp/$(basename "$f" .cpp).${out_suffix}.o"
done

if [ -d examples ]; then
  for f in examples/*.cpp; do
    echo "=== compiling (expect success) $f ==="
    "$compiler" "$@" -c "$f" -o "/tmp/$(basename "$f" .cpp).${out_suffix}.o"
  done
fi

for f in tests/expected_failures/*.cpp; do
  echo "=== compiling (expect failure) $f ==="
  case "$f" in
    *wrong_shape_utf8_policy.cpp)
      expect_substr="Utf8Policy must provide static bool is_valid" ;;
    *wrong_shape_duplicate_key_policy.cpp)
      expect_substr="KeyPolicy must provide static bool on_duplicate" ;;
    *)
      expect_substr="" ;;
  esac
  if output=$("$compiler" "$@" -c "$f" -o /tmp/expected_failure.o 2>&1); then
    echo "ERROR: $f was expected to fail to compile, but it succeeded"
    exit 1
  fi
  echo "$output"
  if [ -n "$expect_substr" ]; then
    case "$output" in
      *"$expect_substr"*) ;;
      *)
        echo "ERROR: expected substring '$expect_substr' not found in diagnostic for $f"
        exit 1 ;;
    esac
  fi
done

echo "all tests/*.cpp and examples/*.cpp compiled; all tests/expected_failures/*.cpp correctly failed to compile"
