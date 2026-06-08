FROM alpine:3.20

# Pinned upstream commits — see benchmarks/SOURCES.md
# NOTE: The wren standalone CLI was removed from the wren repo around 2022.
# The current repo only builds wren_test (the test runner), not a standalone interpreter.
# This Dockerfile is kept for future use if a standalone wren CLI becomes available again.
# The Wren suite benchmarks currently run Lox++ only.
ARG WREN_COMMIT=99d2f0b8fc2686134b32b18166e037639f7e9f2c

RUN apk add --no-cache curl cmake make gcc musl-dev

# Build wren from source at pinned commit
# The Makefile is in projects/make/, not the root.
RUN curl -fsSL "https://github.com/wren-lang/wren/archive/${WREN_COMMIT}.tar.gz" \
    | tar xz && \
    cd "wren-${WREN_COMMIT}" && \
    make -C projects/make && \
    cp bin/wren_test /usr/local/bin/wren_test && \
    cd / && rm -rf "wren-${WREN_COMMIT}"

# Fetch Wren benchmark files at the same pinned commit
RUN mkdir -p /tmp/wren /benchmarks/wren && \
    curl -fsSL "https://github.com/wren-lang/wren/archive/${WREN_COMMIT}.tar.gz" \
    | tar xz --strip-components=1 -C /tmp/wren && \
    cp -r /tmp/wren/test/benchmark/. /benchmarks/wren/ && \
    rm -rf /tmp/wren

WORKDIR /benchmarks
