FROM alpine:3.20

# Pinned upstream commits — see benchmarks/SOURCES.md
ARG AWFY_COMMIT=74306fec151070fd07157cefeacf19e7e0bcdc89

RUN apk add --no-cache lua5.4 curl

# Fetch AWFY Lua benchmarks at pinned commit
# Note: Alpine uses busybox tar (no --wildcards); extract all, then select subdir.
RUN mkdir -p /tmp/awfy /benchmarks/awfy && \
    curl -fsSL "https://github.com/smarr/are-we-fast-yet/archive/${AWFY_COMMIT}.tar.gz" \
    | tar xz --strip-components=1 -C /tmp/awfy && \
    mv /tmp/awfy/benchmarks/Lua/* /benchmarks/awfy/ && \
    rm -rf /tmp/awfy

# Wrapper: invokes a single AWFY benchmark via the harness
COPY benchmarks/containers/run_awfy_lua.sh /benchmarks/run_bench.sh
RUN chmod +x /benchmarks/run_bench.sh

WORKDIR /benchmarks
