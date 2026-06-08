FROM node:22-alpine

# Pinned upstream commits — see benchmarks/SOURCES.md
ARG AWFY_COMMIT=74306fec151070fd07157cefeacf19e7e0bcdc89

RUN apk add --no-cache curl

# Fetch AWFY JavaScript benchmarks at pinned commit
# Note: Alpine uses busybox tar (no --wildcards); extract all, then select subdir.
RUN mkdir -p /tmp/awfy /benchmarks/awfy && \
    curl -fsSL "https://github.com/smarr/are-we-fast-yet/archive/${AWFY_COMMIT}.tar.gz" \
    | tar xz --strip-components=1 -C /tmp/awfy && \
    mv /tmp/awfy/benchmarks/JavaScript/* /benchmarks/awfy/ && \
    rm -rf /tmp/awfy

# Wrapper: invokes a single AWFY benchmark via the Run harness
COPY benchmarks/containers/run_awfy_js.js /benchmarks/run_bench.js

WORKDIR /benchmarks
