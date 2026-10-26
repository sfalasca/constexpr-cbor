# syntax=docker/dockerfile:1
# Builds the browser viewer's WebAssembly decoder and serves the whole
# directory (index.html, app.js, dist/) over plain HTTP, for both local
# "just let me open it" use and as a CI build-smoke-test.
#
# Build + run locally:
#   docker build -t cbor-webapp -f webapp/Dockerfile .
#   docker run --rm -p 8080:8080 cbor-webapp
#   # then open http://localhost:8080 in a browser
#
# CI only builds the image (proving bridge.cpp compiles clean against the
# real Emscripten toolchain with the same warning set the rest of this
# project uses) -- it doesn't need to actually serve anything to count as
# passing.
FROM emscripten/emsdk:latest AS build
WORKDIR /src
COPY include/ include/
COPY webapp/ webapp/
RUN webapp/build.sh

FROM python:3-slim
WORKDIR /site
COPY webapp/index.html webapp/app.js ./
COPY webapp/README.md ./
COPY --from=build /src/webapp/dist/ ./dist/
EXPOSE 8080
CMD ["python3", "-m", "http.server", "8080"]
