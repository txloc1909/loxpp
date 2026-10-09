# Import system: design-space note (v0)

**Status: the minimal viable shape below is approved as the frame, plus
explicit exports. Node issues and a tracking issue follow.** This note maps the design space for an import system under a fixed
set of constraints, per `AGENTS.md`'s planning policy, so the trade-offs are on
the table before any code lands. It does **not** pick a final design; it bounds
the space and names the residual hard edges.

## Why this note

`expressiveness-roadmap.md` rules modules **out of the capability list**:
"Modules / namespaces — organizational, not expressive. One file can express
any program; modules scale code, they don't add capability. (Dynamic *loading*
is the exception — that's `eval`, deferred.)" So an import system is never a
language-gap mission; it is an organizational feature, and its whole job is to
scale code **without** reopening the deferred `eval` / compiler-re-entrancy
wall. This note records what that leaves on the table.

## Settled constraints (the frame)

1. Single-binary distribution (`distribution.md`): one self-contained binary
   (plus `libloxrt.a` beside it, used only by `--target qbe`),
   `loxpp upgrade` swaps the binary atomically.
2. Every program is a single whole program. Imports are **compile-time
   composition**; the compiled artifact is one program, not separately-loaded
   runtime units.
3. Imports resolve **statically, at compile time**. No runtime/dynamic loading.
4. Runtimepath is **static and per-interpreter** — resolved once at startup,
   immutable during a run.

## What the constraints take off the table

These are the earlier hard problems the frame eliminates, and why:

- **No `eval` wall.** Compile-time resolution needs no compiler call from a
  running program. The VM has a bounded re-entrant call path
  (`concurrency-model-decision.md` C3), but it only calls already-compiled
  code; nothing can invoke the compiler at run time. Dynamic/lazy import,
  hot-reload, and import-of-a-runtime-path stay out of scope by construction.
- **No QBE separate-compilation/linker problem.** One whole-program artifact
  keeps the whole-program assumption native/QBE already rely on.
- **No per-import runtime path search and no cwd-drift at run time.**
- **Concurrency/actor tension mostly defused.** One whole-program artifact has
  no cross-task shared module singletons to reason about (see
  `actor-model-design.md`); revisit only if item 7 adopts per-task VMs.

What remains is a small, bounded space: six axes.

## Axis 1 — resolution & naming

Two sub-choices: what an import names, and what it is anchored to.

- **Logical name vs. path.** `import math` (logical name, resolved against the
  runtimepath) vs. `import "./util.lox"` (path). Logical names suit shipped
  stdlib; paths suit a program's own local files. A syntactic tell (quoted
  string = path, bare identifier = runtimepath name) keeps both unambiguous.
- **Anchor — not cwd.** cwd-relative imports make a program's meaning depend on
  the directory it is invoked from (the Python `sys.path[0]` footgun): the same
  script resolves different files from different shells. Robust pairing:
  **local imports resolve relative to the importing file's own directory**, and
  **logical imports resolve against a fixed runtimepath**. cwd then anchors
  nothing.

## Axis 2 — binding model

The codebase already has the ideal lowering: `math` and `coroutine` are globals
whose members are reached by `GET_PROPERTY` with a **constant** name operand.
Backend-uniform, **no new opcode**.

- **Module-as-object:** `import math` binds one module-object global; members
  via `.`. Path of least resistance; works on all four backends immediately.
- **Selective (`from m import f`):** binds names directly into the importer's
  scope. Static resolution makes this cheap (exported names are known at
  compile time) — compiler-side binding, still no new opcode, but it adds
  collision/shadowing rules.
- **Export control:** implicit (all top-level names public) is simplest but
  leaks helpers; explicit (`export`/`pub`) costs one keyword across the surface
  but gives an interface — the natural carrier for a future `may-suspend`
  summary (see residual edges).
- Reject the `#include`-style whole-namespace merge; it is the collision-prone
  option.

## Axis 3 — composition / linking

- **Source-inclusion:** resolve each import to source, compile the transitive
  closure into one chunk tree. Zero new artifact formats; stays in the
  representation all backends already consume.
- **Compile-then-link chunks:** compile each module to a chunk, then link
  cross-module global refs. More structure, and the only path that enables
  shipping precompiled modules (Axis 5b) — but it needs a serialized-chunk
  format and loader that **do not exist today**.
- **Dedup is mandatory either way:** a diamond import compiles/initializes a
  module **once** — a module cache keyed by resolved identity.

## Axis 4 — initialization order & cycles

A module's top-level is still code that runs. Because resolution is static, the
clean option is available: treat imports as declarations forming a DAG and make
an **import cycle a compile-time error**. That deletes the partial-init footgun
entirely — no half-loaded-module visibility to specify, nothing for four
backends to agree on at run time. With **single-initialization** (each module's
top-level runs once, in topological order), the whole init story becomes a
compile-time, deterministic, trivially-cross-backend thing. Highest
design payoff hiding in the constraints.

## Axis 5 — shipping pure-Lox++ stdlib (and the libloxrt.a correction)

Intent: ship Lox-level stdlib for things that need no VM/runtime change. Sound,
but **`libloxrt.a` is the wrong model**, for a parity reason:

- `libloxrt.a` (`loxrt`) is the **QBE runtime** — native C object code that
  QBE-*emitted* machine code links against (`rt_capi.*`). It is target-specific
  (x86_64/musl) and serves exactly **one** backend.
- A pure-Lox++ stdlib module must run identically on native, JVM, QBE, and the
  bootstrap. Precompiling it to native produces an artifact only the QBE/native
  target can consume — it cannot feed the JVM or the self-hosted
  interpreter. So precompile-to-native **breaks the portability that makes a
  Lox-level stdlib worth shipping.**

Two options preserve four-backend parity:

- **(a) Embed module source** as read-only blobs in the binary, exposed as a
  built-in namespace on the runtimepath; the importer compiles them with the
  user program. Zero new format machinery, trivially portable, keeps the
  single-binary promise. Cost: recompiling stdlib each run — negligible for a
  small stdlib, since Lox compiles fast. **Default choice.**
- **(b) Embed portable serialized bytecode** (chunks), skipping stdlib
  recompilation at startup. Faster cold start, but you then own a stable
  chunk-serialization format, a loader, versioning, and a trust/verification
  surface — none of which exist today. Reach for it only when a benchmark says
  startup cost is real.

## Axis 6 — runtimepath shape

- **Per-interpreter, static:** resolved once at startup from built-in defaults
  plus at most one explicit user entry (a flag or env var), immutable during a
  run. Consistent with compile-time resolution.
- **Precedence** is the rule to pin: built-in stdlib vs. user path on a name
  collision. Cleanest is to make the embedded stdlib a **reserved/anchored
  namespace** user paths cannot shadow (no "my `math.lox` silently shadowed the
  stdlib" surprise).

## The minimal viable shape

Static logical+path imports; file-relative anchoring for local imports; a fixed
per-interpreter runtimepath for logical ones; module-as-object binding;
source-inclusion composition with a dedup cache; cycles-as-compile-error with
single-initialization; and an embedded-**source** stdlib namespace. Every
richer choice — selective import, explicit exports, serialized-bytecode stdlib,
compile-then-link — is an independent increment on top, each with the cost named
above, and none reopen the `eval` wall.

## Residual hard edges (survive even the minimal model)

1. **The coroutine/QBE whole-program taint reaches the stdlib.** The obvious
   first Lox-level stdlib modules (lazy/infinite sequences) use `yield`. Under
   whole-program QBE coroutine mode (`coroutines.md`, `treeContainsYield` →
   `coroutineMode`), any program importing a `yield`-using module compiles the
   **whole** program in coroutine mode (promotion off, trampoline). "Ship
   generator-based stdlib" and "QBE stays fast" are in tension; it argues for a
   `may-suspend` effect analysis with Axis-2 explicit exports as the summary
   carrier.
2. **The bootstrap goes multi-file.** The self-hosted interpreter
   (`bootstrap/loxpp_interpreter.lox`) reads exactly one program today, from
   stdin through `input()` (`bootstrap/lox_wrapper.sh`'s sentinel protocol),
   so it does not even know the main program's path — file-relative anchoring
   needs that path passed in. Even static multi-file resolution means it must
   resolve paths, read files with `open()`, and re-enter its own
   scanner/parser on more files —
   far easier than dynamic loading, but the sharpest per-backend lift and the
   one most likely to be under-scoped (the recurring bootstrap blind spot — see
   `multi-agent-playbook.md`, "scope a mission around every consumer").
3. **Bounded tooling tax.** New `import`/`export` syntax across `spec/`
   01/02/04/05, the tree-sitter grammar + queries, three editor syntaxes, a
   cross-file resolver, LSP cross-file go-to-definition + unresolved-import
   diagnostics, and `check_examples.py` / `diff_runtimes.py` learning to run
   multi-file programs. All mechanical, all bounded — but it is the real cost
   envelope.
4. **Determinism of resolution.** Missing-module, ambiguous-resolution, and
   cycle errors are compile-time (good — caught before run), but their messages
   and exit behavior must be identical across all four backends (C5).

## Decisions (2026-10-08)

- The minimal viable shape is the frame for the first mission.
- Exports are explicit: a top-level declaration marked `export` is visible to
  importers. Unmarked names stay private to the module.
- Explicit exports give an interface. They do not make a `may-suspend`
  analysis sound: source inclusion shows the compiler every module, so the
  analysis needs no summary. Residual edge 1 is a stack-ownership question
  that `concurrency-model-decision.md` item 6 must answer. A generator-based
  stdlib module must not ship before that answer.
- Import resolution and FFI `.so` loading share one loadable-unit and
  search-path design: the same runtimepath, the same anchoring rule, and the
  same reserved stdlib namespace. The FFI mission starts from this design.
- Combinator libraries (`Option`, `Result`) are modules whose functions are
  chained with a pipe operator. See `dynamic-functional-programming.md`.

## Two kinds of stdlib

The stdlib has two kinds of module:

1. **Native builtins**, compiled into the native VM. These are the only kind
   today: about thirty flat globals (`clock`, `stat`, `spawn`, `connect`,
   `type`, and others) and two namespace objects (`math`, `coroutine`).
2. **Lox++ modules**, written in Lox++ and bundled with the native VM as
   embedded source (Axis 5a). None exist yet.

Six rules keep the two kinds consistent.

- **One namespace for both kinds.** `import X` finds `X` in the reserved
  stdlib namespace, whether it is native or written in Lox++. A program
  cannot tell which. A module can therefore move between the kinds with no
  change to any program: a hot Lox++ module can become native, and a native
  module can be rewritten in Lox++. The native VM registers native modules in
  a module table, not only as globals.
- **A small prelude.** Only a short core stays global with no import. The
  proposed core is `clock`, `str`, `ord`, `chr`, `input`, `type`, and the
  error and protocol basics. This list needs a decision. The other builtins
  move into native modules, for example `os`, `fs`, `net`, `process`,
  `reflect`, `math` and `coroutine`. This removes the flat-namespace problem
  that forces names such as `mapResult`. No Lox++ module is in the prelude,
  so a program compiles only the modules it imports.
- **Hybrid modules.** A Lox++ module can sit on top of a small native core, as
  Python's `json` sits on `_json`. Native modules whose names start with an
  underscore are private: only stdlib modules can import them.
- **A placement rule.** A function is native only if it cannot be written in
  Lox++ (OS access, reflection, coroutines) or a measurement shows that it is
  hot. Everything else is written in Lox++. This is the litmus test of
  `expressiveness-roadmap.md` applied to the stdlib.
- **Spec layout.** `spec/05-stdlib.md` has one section for the prelude and one
  section per module. A Lox++ module is specified by its interface, the same
  way as a native module. Its source is the implementation, not the spec.
- **Bootstrap access to stdlib source.** The native compiler does source
  inclusion, so the JVM and QBE backends receive stdlib modules inside the
  composed bytecode. The bootstrap interpreter has its own frontend and cannot
  read source embedded in the binary. An internal builtin returns an embedded
  module's source text by name, in the same way that the bootstrap already
  delegates I/O to its host.

Lox++ modules live in one repository directory, and a build step embeds them
in the binary. They have the same version as the binary, so `loxpp upgrade`
updates both. The full package (`distribution.md`, "Two packages") may also
carry a precompiled cache of them. The source stays the reference.

## Non-goals

- Dynamic / runtime import, lazy import, hot-reload, `eval`. Those are the
  deferred capability; this note stays behind that wall deliberately.
- A native-extension ABI (`.so` loading) — roadmap item 6, sequenced after the
  concurrency model. A Lox-level import system and the native-extension loader
  will want a shared "loadable unit + search path" story; design this one with
  an eye to that, so there are not two divergent runtimepaths later.

## References

- `expressiveness-roadmap.md` — modules are organizational; dynamic loading is
  `eval`, deferred.
- `distribution.md` — single static binary; `libloxrt.a` / `loxrt` component.
- `coroutines.md` — whole-program QBE coroutine mode (the taint in edge 1).
- `bytecode-translation-problems.md` — the one-chunk shared pipeline all
  backends consume.
- `concurrency-model-decision.md`, `actor-model-design.md` — per-task VM /
  share-nothing, for the eventual item-7 interaction.
- `multi-agent-playbook.md` ("scope a mission around every consumer") and
  `coroutines-retro.md` (subset closure, issue #564) — the lessons any import
  mission must apply before scoping nodes.
