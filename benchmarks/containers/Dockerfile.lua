FROM alpine:3.20

# Pinned upstream commits — see benchmarks/SOURCES.md
ARG AWFY_COMMIT=5e9fa8e657a4b9c4b3dfb4e9a2ac31b5ad3b0ef3

RUN apk add --no-cache lua5.4 curl

# Fetch AWFY Lua benchmarks at pinned commit
RUN mkdir -p /benchmarks/awfy && \
    curl -fsSL "https://github.com/smarr/are-we-fast-yet/archive/${AWFY_COMMIT}.tar.gz" \
    | tar xz --strip-components=2 \
         --wildcards \
         -C /benchmarks/awfy \
         "are-we-fast-yet-${AWFY_COMMIT}/benchmarks/Lua/*"

WORKDIR /benchmarks
