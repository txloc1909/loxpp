FROM alpine:3.20

# lua5.4 interpreter + AWFY Lua benchmarks/harness. Serves both harnesses:
# crosslang.py mounts its micro ports at /bench; runner.py uses the AWFY suite
# baked at /benchmarks/awfy (run via harness.lua). GNU tar avoids busybox's
# missing --strip-components/dir-extraction quirks. See SOURCES.md.
ARG AWFY_COMMIT=74306fec151070fd07157cefeacf19e7e0bcdc89

RUN apk add --no-cache lua5.4 curl tar

RUN mkdir -p /benchmarks/awfy && \
    curl -fsSL "https://github.com/smarr/are-we-fast-yet/archive/${AWFY_COMMIT}.tar.gz" \
    | tar xz -C /benchmarks/awfy --strip-components=3 \
        "are-we-fast-yet-${AWFY_COMMIT}/benchmarks/Lua"

# harness.lua require()s modules relative to the working directory.
WORKDIR /benchmarks/awfy
