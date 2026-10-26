#!/usr/bin/env bash
# Builds dist/cbor.js + dist/cbor.wasm from bridge.cpp. Needs the
# Emscripten SDK on PATH (em++); if you don't have it installed locally,
# run this inside the emscripten/emsdk image instead (see Dockerfile in
# this directory, or docker/wasm.Dockerfile's own emcmake/emmake pattern
# one level up).
#
# Usage: webapp/build.sh
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "${script_dir}"

mkdir -p dist

em++ -std=c++23 -O2 -I ../include \
    -sMODULARIZE=1 -sEXPORT_NAME=CborModule \
    -sEXPORTED_FUNCTIONS=_cbor_to_json,_cbor_free_string,_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,UTF8ToString \
    -sALLOW_MEMORY_GROWTH=1 \
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror \
    bridge.cpp -o dist/cbor.js

echo "webapp/build.sh: built dist/cbor.js + dist/cbor.wasm"
