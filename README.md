# Lox++

[![CI](https://github.com/txloc1909/loxpp/actions/workflows/ci.yml/badge.svg)](https://github.com/txloc1909/loxpp/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)

A bytecode-compiled virtual machine for the Lox++ scripting language, written
in C++17.

The `++` is both a nod to the implementation language and a tribute to
[Lox](https://craftinginterpreters.com). Lox++ is its own language, shaped by
the same philosophy but taken further with pattern matching, algebraic types,
destructuring, collections, file I/O, and a self-hosted bootstrap interpreter.

---

## Features

| Feature | What it gives you |
|---|---|
| `enum` + `match` | Algebraic data types with exhaustive pattern matching |
| Destructuring | Unpack class fields (`var {x, y} = pt`) and sequences (`var [a, b] = lst`) |
| Lists & Maps | First-class dynamic collections with slice syntax (`lst[1:3]`) |
| Closures + GC | Lexical closures with mark-and-sweep garbage collection |
| `for-in` / `break` / `continue` | Iterator protocol over any sequence or map |
| File I/O | `open()`, `.read()`, `.write()`, `.close()` |
| Math library | `math.sqrt`, `math.pow`, `math.floor`, `math.pi`, … |
| Self-hosted | A complete Lox++ interpreter [written in Lox++](bootstrap/loxpp_interpreter.lox) |

---

## Quick demo

```lox
// Algebraic types with pattern matching
enum Result { Ok(value)  Err(msg) }

fun safe_div(a, b) {
    if (b == 0) return Err("division by zero");
    return Ok(a / b);
}

// Closures as first-class values
fun makeMultiplier(factor) {
    fun mult(x) { return x * factor; }
    return mult;
}

var double = makeMultiplier(2);

// Lists, for-in, and built-ins
var nums = [1, 2, 3, 4, 5, 6];
var evens = [];
for (n in nums) {
    if (n % 2 == 0) evens.append(double(n));
}

// Pattern match on enum variants
for (n in evens) {
    print match safe_div(60, n) {
        case Ok(v)  => str(60) + " / " + str(n) + " = " + str(v)
        case Err(m) => "error: " + m
    };
}
// 60 / 4 = 15
// 60 / 8 = 7.5
// 60 / 12 = 5
```

More programs in [`examples/`](examples/) — from graph traversal and Huffman
coding to a [self-hosted interpreter](bootstrap/loxpp_interpreter.lox).

---

## Language tour

| Resource | Contents |
|---|---|
| [`spec/`](spec/) | Full language specification — lexical rules, grammar, type system, semantics, stdlib |
| [`examples/`](examples/) | 40+ runnable programs: algorithms, OOP, pattern matching, file I/O |
| [`bootstrap/loxpp_interpreter.lox`](bootstrap/loxpp_interpreter.lox) | A complete Lox++ interpreter written in Lox++ itself |

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for setup, workflow, and conventions.
AI agents: see [AGENTS.md](AGENTS.md) for the complete task loop.

---

## License

[MIT](LICENSE) — Copyright 2025 Tran Xuan Loc
