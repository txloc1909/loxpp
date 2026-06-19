FROM alpine:3.20

# Lua 5.4 interpreter for the apples-to-apples cross-language microbenchmarks
# (benchmarks/crosslang.py). The benchmark programs are mounted at run time
# from benchmarks/lua/, so nothing else is baked in.
RUN apk add --no-cache lua5.4

WORKDIR /bench
