FROM ubuntu:24.04 AS dev

ENV DEBIAN_FRONTEND=noninteractive

# This stage carries clang 18 (glibc). The release-static stage carries
# clang 20 (musl Alpine 3.22). These majors are recorded here so a future
# version bump moves both stages together. No released Alpine matches clang 18;
# alignment requires a newer Ubuntu base or an Alpine downgrade.
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

# ccache is wired in by CMakeLists.txt (CMAKE_*_COMPILER_LAUNCHER). Point the
# cache at /ccache so a `-v <volume>:/ccache` mount persists it across the
# ephemeral agent containers; the size is set here rather than with `ccache
# -M` because a mounted CCACHE_DIR bypasses any baked ccache.conf. 10G covers
# the object variants across the debug (ASan+UBSan), release, and *-variant
# (LOXPP_NAN_TAGGING off) presets.
ENV CCACHE_DIR=/ccache
ENV CCACHE_MAXSIZE=10G

WORKDIR /workspace

# --- dev-editors -----------------------------------------------------------
# Adds Node.js and the tree-sitter CLI for the editors/tree-sitter-loxpp
# grammar. Kept out of `dev` so the C++-only jobs do not load a JavaScript
# runtime they never use. This stage also adds Neovim for the plugin smoke
# test.
FROM dev AS dev-editors

# Node.js 22 LTS from NodeSource. Ubuntu 24.04 ships an older Node in its own
# archive; the tree-sitter CLI needs a current LTS.
RUN curl -fsSL https://deb.nodesource.com/setup_22.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# Pinned so a grammar regeneration in CI matches what a contributor runs
# locally. The CLI version sets the generated parser ABI and the test
# harness behaviour.
RUN npm install -g tree-sitter-cli@0.25.10

# Neovim for the editors/loxpp.nvim headless smoke test. Ubuntu 24.04 ships
# 0.9.5, which is older than the >= 0.11 the plugin needs for the
# vim.lsp.config / vim.lsp.enable path, so take the upstream stable tarball at
# a pinned version. The release asset is nvim-linux-x86_64.tar.gz from 0.10.4
# onward (it was nvim-linux64.tar.gz before).
RUN curl -fsSL "https://github.com/neovim/neovim/releases/download/v0.11.3/nvim-linux-x86_64.tar.gz" \
        -o /tmp/nvim.tar.gz \
    && echo "02b808a3ee8fc30161e07fe3c3edfb24b28bd0295323ac5dbdd8ec7012cac67d  /tmp/nvim.tar.gz" \
        | sha256sum -c - \
    && tar -xzf /tmp/nvim.tar.gz -C /opt \
    && ln -s /opt/nvim-linux-x86_64/bin/nvim /usr/local/bin/nvim \
    && rm /tmp/nvim.tar.gz

# Pinned checkouts of the two optional plugin dependencies the headless test
# and :checkhealth exercise. The plugin does NOT hard-depend on either:
# tree-sitter highlighting needs only a compiled parser on 'runtimepath'
# (tools/check_nvim_plugin.sh builds one directly), and the `loxpp --check`
# fallback needs nvim-lint only in --fallback mode. nvim-treesitter is kept
# on its `master` branch (classic get_parser_configs API); the plugin also
# works with the `main` rewrite or with neither.
RUN git clone --branch master https://github.com/nvim-treesitter/nvim-treesitter /opt/nvim-plugins/nvim-treesitter \
    && git -C /opt/nvim-plugins/nvim-treesitter checkout cf12346a3414fa1b06af75c79faebe7f76df080a \
    && git clone https://github.com/mfussenegger/nvim-lint /opt/nvim-plugins/nvim-lint \
    && git -C /opt/nvim-plugins/nvim-lint checkout 3d55c8f67c6ae5c15e1042571e107c7a3d5c5f4e \
    && rm -rf /opt/nvim-plugins/*/.git

# --- dev-managed ------------------------------------------------------------
# Adds the JVM and CLR toolchains needed by the --target jvm / --target clr
# backends. Kept out of `dev` so the C++-only jobs (lint, build matrix,
# clang-tidy) don't pay to load ~1 GB of managed runtimes they never use.
# The CLR half sits below the JVM half, so bumping .NET or ilasm leaves the JVM
# layers cached. Layers invalidate downward, so the reverse does not hold.
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

# Ubuntu 24.04 carries the .NET SDK in its own archive, so no Microsoft feed and
# no third-party apt key to maintain.
RUN apt-get update && apt-get install -y \
    dotnet-sdk-8.0 \
    && rm -rf /var/lib/apt/lists/*

# ilasm assembles the .il text CIL the CLR backend emits. It is not part of the
# .NET SDK on Linux — Microsoft ships it only inside a runtime-specific NuGet
# package — so unpack the binary out of that. nuget.org will not let a published
# version be replaced, but verify the digest anyway to match the jasmin fetch.
RUN curl -fsSL "https://api.nuget.org/v3-flatcontainer/runtime.linux-x64.microsoft.netcore.ilasm/8.0.0/runtime.linux-x64.microsoft.netcore.ilasm.8.0.0.nupkg" \
        -o /tmp/ilasm.nupkg \
    && echo "e7c3c4a9a082a11c7e91ce74ba5dad83a8877f4ed85d5f8e1f2c9ea6c2cadee7  /tmp/ilasm.nupkg" \
        | sha256sum -c - \
    && unzip -q /tmp/ilasm.nupkg -d /tmp/ilasm \
    && install -Dm755 /tmp/ilasm/runtimes/linux-x64/native/ilasm \
        /usr/local/bin/ilasm \
    && rm -rf /tmp/ilasm.nupkg /tmp/ilasm

# --- release-static --------------------------------------------------------
# Alpine 3.22 with musl libc for a fully static build. Separate stage, same
# Dockerfile, independent of the glibc dev stages above. See "dev image's
# clang" comment below.
FROM alpine:3.22@sha256:14358309a308569c32bdc37e2e0e9694be33a9d99e68afb0f5ff33cc1f695dce AS release-static

# Alpine 3.22 carries clang 20.1.8 (musl). The dev stage on Ubuntu 24.04
# carries clang 18 (glibc). The majors do not match. A future alignment
# requires a newer Ubuntu with clang 20 or a pins downgrade to Alpine 3.21
# (clang 19, ~4 weeks support left as of 2026-09). The durable fix is a
# newer Ubuntu base when released. Record both majors so a bump is deliberate.
RUN apk add --no-cache \
    clang lld cmake ninja make g++ musl-dev libstdc++-dev linux-headers \
    binutils python3 git

# ccache is shared across all stages (see dev stage comment). The
# release-static build produces both glibc and musl object variants; raise
# CCACHE_MAXSIZE to 20G to avoid contention and evictions.
ENV CCACHE_DIR=/ccache
ENV CCACHE_MAXSIZE=20G

WORKDIR /workspace
