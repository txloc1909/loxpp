FROM alpine:3.20

# Pinned upstream commits — see benchmarks/SOURCES.md
ARG WREN_COMMIT=4a8e084b8a46e2f7b98c5b9ee1b44e91a8eef9c7

RUN apk add --no-cache curl cmake make gcc musl-dev

# Build wren from source at pinned commit
RUN curl -fsSL "https://github.com/wren-lang/wren/archive/${WREN_COMMIT}.tar.gz" \
    | tar xz && \
    cd "wren-${WREN_COMMIT}" && \
    make && \
    mv wren /usr/local/bin/wren && \
    cd / && rm -rf "wren-${WREN_COMMIT}"

# Fetch Wren benchmark files at the same pinned commit
RUN mkdir -p /benchmarks/wren && \
    curl -fsSL "https://github.com/wren-lang/wren/archive/${WREN_COMMIT}.tar.gz" \
    | tar xz --strip-components=3 \
         --wildcards \
         -C /benchmarks/wren \
         "wren-${WREN_COMMIT}/test/benchmark/*"

WORKDIR /benchmarks
