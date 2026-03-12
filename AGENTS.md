# AGENTS.md 

## Overview
Lox is a dynamically-typed scripting language. 
Lox++ is an bytecode compiler & VM for Lox, written in C++.

## Dev environment tip

Containerized dev env:
```bash
podman build -t loxpp-dev-env .
# Create new box
distrobox create --name loxpp-dev --image loxpp-dev-env
# Or enter existing one
distrobox enter loxpp-dev
```

Build & test & run:
```bash
# Build
cmake -B build -G Ninja && cmake --build build
# Test
ctest --test-dir build
# Run in interactive mode
./build/loxpp
```
