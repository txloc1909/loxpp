FROM ubuntu:24.04 AS dev

ENV DEBIAN_FRONTEND=noninteractive

# Install toolchain
RUN apt-get update && apt-get install -y \
    clang \
    lld \
    cmake \
    ninja-build \
    git \
    gdb \
    build-essential \
    libgtest-dev \
    ccache \
    clang-format \
    clang-tidy \
    pkg-config \
    libreadline-dev \
    python3 \
    curl \
    unzip \
    && rm -rf /var/lib/apt/lists/*

# Install Dart 2.19.6 (craftinginterpreters test runner requires <3.0.0)
RUN curl -fsSL "https://storage.googleapis.com/dart-archive/channels/stable/release/2.19.6/sdk/dartsdk-linux-x64-release.zip" \
        -o /tmp/dart.zip \
    && unzip -q /tmp/dart.zip -d /usr/local \
    && rm /tmp/dart.zip

ENV PATH="/usr/local/dart-sdk/bin:${PATH}"

# Build and install gtest static libs (Ubuntu ships source only)
RUN cd /usr/src/gtest && \
    cmake -B build -S . && \
    cmake --build build && \
    cp build/lib/*.a /usr/lib/

ENV CC="ccache clang"
ENV CXX="ccache clang++"

RUN ccache --max-size=2G

WORKDIR /workspace

# --- dev-managed ------------------------------------------------------------
# Adds the JVM and CLR toolchains needed by the --target jvm / --target clr
# backends. Kept out of `dev` so the C++-only jobs (lint, build matrix,
# clang-tidy) don't pay to load ~1 GB of managed runtimes they never use.
# Each toolchain gets its own layer so bumping one doesn't rebuild the other.
FROM dev AS dev-managed

# OpenJDK 21 (LTS). Only the major version is pinned; patch releases track the
# Ubuntu archive. HotSpot's version is part of any performance baseline
# measured through this backend, so check_managed_toolchains.sh reports the
# exact build for the record.
RUN apt-get update && apt-get install -y \
    openjdk-21-jdk-headless \
    && rm -rf /var/lib/apt/lists/*

# Jasmin assembles the .j text bytecode the JVM backend emits. Upstream is a
# 2010 SourceForge release reached through redirecting mirrors, so verify the
# archive rather than trusting the transport.
RUN curl -fsSL "https://downloads.sourceforge.net/project/jasmin/jasmin/jasmin-2.4/jasmin-2.4.zip" \
        -o /tmp/jasmin.zip \
    && echo "eaa10c68cec68206fd102e9ec7113739eccd790108a1b95a6e8c3e93f20e449d  /tmp/jasmin.zip" \
        | sha256sum -c - \
    && unzip -q /tmp/jasmin.zip -d /tmp/jasmin \
    && install -Dm644 /tmp/jasmin/jasmin-2.4/jasmin.jar /opt/jasmin/jasmin.jar \
    && printf '#!/bin/sh\nexec java -jar /opt/jasmin/jasmin.jar "$@"\n' \
        > /usr/local/bin/jasmin \
    && chmod +x /usr/local/bin/jasmin \
    && rm -rf /tmp/jasmin.zip /tmp/jasmin
