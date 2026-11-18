# A program declaration and a built-in global with the same name

**Status:** proposal. Waits for the owner's decision on #781. It does not
open work: the rule takes effect with the import-system mission
(`docs/decisions/import-system.md`), which defines the prelude.

## Problem

A program can declare a name that a built-in global already has. Today the
result depends on the kind of declaration (native, release build of `main`
after mission #771):

| Declaration | Effect on the built-in | Example |
|---|---|---|
| global `var`, `fun`, `class` | Replaced from the point where the declaration runs. A use earlier in the run still reaches the built-in. | `print type(1); var type = 1;` prints `Number` |
| local `var`, parameter, pattern binding, `catch` or `for`-in variable | Hidden in its scope only (lexical). | `fun h(clock) { return clock; }` |
| `enum` | Hidden in the whole program, also before the declaration (`spec/02-syntax.md`, enum declaration, rule from #780). | `enum type { A }` makes every `type(x)` a compile error |
| `str`, `len` | Not possible: both are keywords. | `var str = 1;` is "Expect variable name." |

Three rules for one question. Two problems follow:

- **A silent break.** `var type = typeOf(x);` at the top level of one part of
  a program breaks every later `type(v)` call in another part, with no
  diagnostic. The program fails far from the cause, or gives a wrong result.
  This is the kind of fault that `VISION.md` asks to fail loudly.
- **The spec states only one rule.** `spec/02-syntax.md` describes the enum
  rule. The rule for the other kinds is only what native does.

The owner allowed the current state on 2026-10-21 (review of PR #780) until
the import-system mission, because two facts change with it:

1. Most built-ins leave the global namespace. The import decision ("Two kinds
   of stdlib", "A small prelude") keeps only a short core global: `clock`,
   `str`, `ord`, `chr`, `input`, `type`, and the error and protocol basics.
   The others move into native modules (`os`, `fs`, `net`, `process`,
   `reflect`, `math`, `coroutine`) and a program reaches them as members
   (`os.stat`), so a program name cannot collide with them.
2. The prelude becomes a list in the spec. Today the set of built-ins is only
   the native VM's run-time table, so the compiler cannot check a name
   against it without that table.

## Options

### A. Hiding is allowed for every kind, and the spec says so

Keep today's behavior and write it down: a declaration of any kind may have
a prelude name and hides it, with one scope rule. The natural scope rule is
the enum one: a global declaration hides the built-in in the whole program,
so `print type(1); var type = 1;` is a compile error or reads the variable,
not the built-in.

- *For:* no program breaks. The rule is short.
- *Against:* the silent break stays. A reader of `type(v)` must check that no
  declaration in the program took the name.

### B. A prelude name is reserved (recommended)

No declaration of any kind may have a prelude name: a global or local
variable, a function, a class, an enum, a parameter, a pattern binding, an
`@`-binding, a `catch` or `for`-in variable, a destructured name. Each is a
static error, with the same message shape as the enum-name rule
("A variable can't have the name of the built-in 'type'.").

- *For:* one rule for every kind, the same shape as declaration rule 4 of
  `docs/decisions/enum-constructor-namespaces.md` (an enum name is reserved
  in the whole program). `type(v)` always means the built-in. The reserved set
  is small (about ten names) once the prelude is the only global core.
- *Against:* a new prelude name breaks the programs that use it. Every
  Lox++ program is in this repository (`AGENTS.md`), so that change rewrites
  them in the same PR, as for any language change.
- *Cost:* the compiler needs the prelude list. The spec holds it as a table
  (`AGENTS.md`, "Use a table when the spec lists a set"), and a script checks
  that the table matches the names that the native VM registers.

### C. A prelude name is reserved at global scope only

A global declaration may not have a prelude name; a local may hide one in
its scope.

- *For:* the silent break at a distance goes away: a global is the only
  declaration that reaches every part of the program. A local name is
  visible next to its uses.
- *Against:* two rules (global and local) where B has one. A local `type`
  still hides the built-in inside a long function.

## Recommendation

**B**, with the import-system mission. Until that mission, keep today's
behavior; this proposal changes nothing before the prelude exists.

Probe `146_enum_hides_builtin_global.lox` pins the enum case of today's rule.
Under B it becomes a static-error probe (an enum named `type` is rejected),
and probe 156 (a built-in used before an enum with its name) becomes
redundant.

## Consumers

Listed before the work, as `AGENTS.md`, "Plans for a language change", rule
1, asks. The change has one phase (the rule starts with the prelude), so
each consumer has one step.

| Consumer | What changes under B |
|---|---|
| `spec/` | `05-stdlib.md`: the prelude table. `02-syntax.md`: the reserved-name rule, next to the enum-name rule. |
| Native compiler | The declaration sites that call `rejectEnumName` today also check the prelude table. |
| JVM, QBE | None: they use the native compiler. |
| Bootstrap interpreter | The resolver's `declare` checks the same table (the bootstrap delegates built-ins to its host, so the table is a list in its source). |
| LSP tooling | The resolver warns on a declaration with a prelude name, as the compiler's diagnostic already shows in the editor. |
| tree-sitter, editor syntaxes | None for the rule. They may highlight prelude names as built-ins. |
| Programs | Every program that declares a prelude name: a search of `.lox` files and of programs embedded in tests and tools, as in N5 of mission #771. |
| Tools | A script that checks the spec's prelude table against the native VM's registered globals. |

## Open questions for the owner

1. A, B or C?
2. Under B: does the rule cover the error and protocol basics of the prelude
   (for example `Error`), or only the functions?
3. Under B: should a member of a native module (`os.stat`) also be reserved
   as a program global? This proposal says no: a module member is never a
   bare name, so it cannot collide.
