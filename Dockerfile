FROM ubuntu:24.04

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
