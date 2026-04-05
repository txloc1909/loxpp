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
    && rm -rf /var/lib/apt/lists/*

# Build and install gtest static libs (Ubuntu ships source only)
RUN cd /usr/src/gtest && \
    cmake -B build -S . && \
    cmake --build build && \
    cp build/lib/*.a /usr/lib/

ENV CC="ccache clang"
ENV CXX="ccache clang++"

RUN ccache --max-size=2G

WORKDIR /workspace
