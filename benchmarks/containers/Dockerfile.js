FROM node:22-alpine

# AWFY (Are We Fast Yet) JavaScript benchmarks + SOM harness — see SOURCES.md.
# Run via: node /benchmarks/awfy/harness.js <BenchmarkName> <iters> <inner>
ARG AWFY_COMMIT=74306fec151070fd07157cefeacf19e7e0bcdc89

RUN apk add --no-cache curl tar

RUN mkdir -p /benchmarks/awfy && \
    curl -fsSL "https://github.com/smarr/are-we-fast-yet/archive/${AWFY_COMMIT}.tar.gz" \
    | tar xz -C /benchmarks/awfy --strip-components=3 \
        "are-we-fast-yet-${AWFY_COMMIT}/benchmarks/JavaScript"

WORKDIR /benchmarks/awfy
