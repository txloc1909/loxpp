FROM ubuntu:24.04

# The `wren` CLI lives in wren-lang/wren-cli (the wren-lang/wren repo builds
# only a test runner). wren-cli bundles the wren VM and libuv as submodules.
# Pinned commit — see benchmarks/SOURCES.md.
ARG WREN_CLI_COMMIT=18553636618a4d33f10af9b5ab92da6431784a8c

RUN apt-get update && apt-get install -y --no-install-recommends \
    git ca-certificates build-essential \
    && rm -rf /var/lib/apt/lists/*

# Build the wren CLI (release, NaN-tagged) from the pinned commit + submodules.
RUN git clone https://github.com/wren-lang/wren-cli /tmp/wren-cli && \
    cd /tmp/wren-cli && \
    git checkout "${WREN_CLI_COMMIT}" && \
    git submodule update --init --recursive && \
    make -C projects/make config=release_64bit -j"$(nproc)" && \
    cp bin/wren_cli /usr/local/bin/wren && \
    cd / && rm -rf /tmp/wren-cli

WORKDIR /bench
