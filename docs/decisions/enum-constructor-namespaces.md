# Enum constructors live in the namespace of their enum

**Status:** accepted. The owner approved it in #768 (2026-10-21). Tracking
issue: #771.

## Owner decisions

The owner answered the four open questions on 2026-10-21.

| Question | Decision |
|---|---|
| Strict qualification, or bare names when unique | Strict. A pattern and an expression always write `E.A`. |
| `str` shows the enum name | No. `str` gives `<A>` and `A(1)`, as today. |
| `E` alone is a value | No. The import mission can change this if module access needs it. |
| Remove the literal comma chain | Yes, option A. `or` joins every alternative. See "The comma chain". |
| A constructor pattern tested against a value that is not an enum value (review of #780) | It does not match, and the match goes on. The halting fault is removed. Alternatives are tried from left to right. |
| Other declarations with the enum name (review of #780) | No variable that the program declares may have the enum name, at any scope and anywhere in the source. A member name (a constructor, a field, a method, a property) may have it (declaration rule 4). Confirmed by the owner on 2026-10-21. |
| An enum with the name of a built-in global (review of #780) | Allowed. The enum name means the enum in the whole program, so it hides the built-in everywhere (declaration rule 5). Confirmed by the owner on 2026-10-21. #781 tracks how the import mission protects built-ins. |

## Summary

An `enum` declaration binds its enum name, not its constructor names. A
program writes a constructor as `E.A`, in an expression and in a pattern:

```lox
enum Option { Some(v) None }
enum Shape { Circle(r) None }       // a second None is valid

var x = Option.Some(1);
print match x {
    case Option.Some(v) => v
    case Option.None    => 0
};
```

A pattern that names a constructor always names its enum too. A bare
identifier in a pattern is never a constructor test.

## Why

The constructor names of all enums share one flat global namespace today.
Four open issues come from that model, and each one asks the owner for a
separate rule:

| Issue | Question today | Cause in the flat model |
|---|---|---|
| #691 | May two enums share a constructor name? | Two constructors with one name write the same global. |
| #696 | Does `case Some` match a value of another enum? | A pattern names a constructor but no enum, so native compares the tag index alone. |
| #747 | Must a positional pattern list every field? | The spec does not say. Native matches a prefix; the bootstrap requires all fields. |
| #756 | What does `case 1 or 2` mean? | The grammar has two arm forms: a literal comma chain, and an identifier `or` chain. |

The flat model also works against two goals in `VISION.md`:

- **Fail loudly.** A bare name in a pattern is a constructor test only when an
  enum constructor of that name is declared earlier in the source. Otherwise
  it is a binding. A typing error such as `case Nnoe` is a binding that
  matches every value. The program runs and gives a wrong result, with no
  fault.
- **Simple.** The pattern rules of `spec/04-semantics.md` must say, for each
  bare name, whether it is a binding, a constructor test, or a class test,
  and the answer depends on the declarations above the match.

The import system (`docs/decisions/import-system.md`) does not fix this. A
module scopes the names of one file, but two enums in one file still share
the namespace. This proposal scopes the names of one enum. The two designs
use the same shape: a name, a `.`, and a member name.

## The design

### Declaration

1. `enum E { ... }` declares the name `E` at global scope. It does not declare
   a global for a constructor.
2. Two constructors of one enum must not have the same name. Two enums must
   not have the same name. Each is a static error, as today.
3. Constructors of different enums may have the same name.
4. No variable that the program declares may have the enum name, at any
   scope and anywhere in the source: a global or local variable, a function,
   a class, another enum, a parameter, a pattern binding, an `@`-binding
   name, a `catch` variable, a `for`-in variable, or a destructured name.
   Each is a static error. Because of this rule, `E` in `E.A` always names
   the enum, and a reader does not have to look for a shadowing declaration.
   A member name (a constructor, a field, a method) is not a variable, so it
   may be the enum name. A built-in global is not declared by the program,
   so an enum may have its name, and the enum hides it in the whole
   program.
5. The enum name means the enum in the whole program. A use of the enum
   name before its declaration in the source, alone or as `E.A`, is a static
   error.

### Expressions

| Form | Meaning |
|---|---|
| `E.A` | The constructor value. It is a value as today: a program can store it, pass it, and call it later. |
| `E.A(x, y)` | A call of the constructor. A wrong number of arguments is the catchable `ConstructorArityError`, as today. |
| `E.B`, where `E` has no constructor `B` | Static error. |
| `E` alone, not followed by `.` | Static error. The enum name is not a value. |
| `E = v`, `E.A = v` | Static error. |
| `A`, where `A` is only a constructor name | An ordinary variable. It reads a variable named `A`, and it is the `UndefinedVariableError` fault when no such variable exists. |

### Patterns

The pattern grammar replaces `identArmPat`, `subPat` and the literal comma
chain:

```
armPats     ::= armPat ( "or" armPat )* ;
armPat      ::= literalPat
              | IDENTIFIER "@" subPat
              | ctorPat
              | IDENTIFIER "{" IDENTIFIER ( "," IDENTIFIER )* "}"   (* class pattern *)
              | IDENTIFIER ;                                       (* "_", binding, or class test *)
subPat      ::= ctorPat
              | IDENTIFIER "{" IDENTIFIER ( "," IDENTIFIER )* "}"
              | IDENTIFIER                                         (* class test *)
              | "[" ( seqElem ( "," seqElem )* )? "]" ;
ctorPat     ::= IDENTIFIER "." IDENTIFIER
                ( "(" IDENTIFIER ( "," IDENTIFIER )* ")"
                | "{" IDENTIFIER ( "," IDENTIFIER )* "}" )? ;
```

The rules:

1. A pattern that has the form `E.A` is a constructor pattern. `E` must be an
   enum declared earlier in the source, and `A` must be a constructor of `E`.
   Otherwise it is a static error.
2. A constructor pattern matches a value only when the constructor `A` of the
   declaration `E` made the value. A value of another enum does not match,
   even when its constructor has the same name or the same position. A value
   that is not an enum value does not match, and the test never faults. An
   enum value that another worker made matches the patterns of the same
   declaration, as today.
3. `E.A` alone tests the constructor and binds no field.
4. `E.A(x, y)` must list one name for each field of `A`. Too many or too
   few names is a static error. A name `_` binds nothing.
5. `E.A{f, g}` binds the named fields. Each name must be a field of `A`, as
   today. It may list a subset of the fields.
6. A bare `IDENTIFIER` is never a constructor test. It is `_`, a binding, or a
   class test. The class rule does not change.
7. `IDENTIFIER "(" ... ")"` with no `.` is no longer a pattern. It is a static
   error.
8. `or` joins any alternatives, literals included: `case 1 or 2 =>` matches 1
   and 2. The comma chain (`case 1, 2 =>`) is removed. One program uses it
   (`examples/match_http_status.lox`); the change rewrites it.
9. The existing rules for alternatives stay. All alternatives must bind the
   same names in the same order, and a bare binding or `_` may be only the
   last alternative.

### Exhaustiveness

The rule does not change in shape. When a match has a constructor pattern of
enum `E`, every constructor of `E` must appear in an arm, or an unguarded `_`
or binding arm must be present. The static error names the missing
constructors. Because rule 2 makes the test per enum, a match that has
patterns of two enums checks each enum.

### What stays

- `type(E.A)` is `"EnumConstructor"`, and `type(E.A(1))` is `"Enum"`.
- Equality of constructors and of enum values is identity, as today.
- The rules for sending an enum value between workers do not change.
- `str` of a constructor and of an enum value does not change (`<A>` and
  `A(1)`).

## How the four issues become trivial

| Issue | Answer under this proposal |
|---|---|
| #691 | Yes. Two enums may share a constructor name (declaration rule 3). The rule needs no decision of its own. |
| #696 | No. A pattern names its enum, so it matches only values of that enum (pattern rule 2). |
| #747 | The pattern must list every field (pattern rule 4). The compiler knows the constructor from `E.A`, so a wrong count is a static error, not a silent mismatch. `_` skips a field, and the `{}` form selects a subset. |
| #756 | `case 1 or 2` matches 1 and 2 (pattern rule 8). The grammar has one alternative operator. |

The change also replaces #752 with a stronger rule: `E` alone becomes a
static error instead of a fault at run time.

## Lowering

This section suggests a lowering. It binds nothing: the spec states only the
behavior above.

- **Constructor values.** The declaration defines one hidden global for each
  constructor, with a name that no identifier can spell, for example `E.A`.
  The compiler resolves `E.A` statically and reads that global. The JVM and
  QBE backends read globals by name already, so they need no new opcode. Each
  worker defines its own constructors when it runs the declarations, as
  today.
- **The enum test.** Give each constructor a tag that is unique in the
  program, not only in its enum. The existing `GET_TAG` and `EQUAL`
  instructions then test the enum and the constructor together, and no
  backend needs a new instruction. `JUMP_TABLE` already subtracts a minimum
  tag, so dense tables still work. The tag is one byte, so a program can
  declare at most 256 constructors in native. That limit is
  implementation-defined (`spec/README.md`, Conformance).
- **Static checks.** The arity check of pattern rule 4 and the name checks
  are compile-time checks in the native compiler and the bootstrap resolver.

## Consumers

| Consumer | What changes |
|---|---|
| `spec/` | `02-syntax.md`: the enum section and the pattern grammar. `04-semantics.md`: match, constructor patterns, exhaustiveness, and "Enum Types". That section describes `GET_TAG` and object fields; the edit replaces it with observable behavior. `03-types.md`, `05-stdlib.md`: examples. |
| Native compiler | Enum declaration, `E.A` in a primary expression, the pattern parser, the exhaustiveness check, program-unique tags. |
| Native VM | None, if the lowering above holds. |
| JVM backend | None, if the lowering above holds. The node proves it with the probes. |
| QBE backend | None, if the lowering above holds. The node proves it with the probes. |
| Bootstrap interpreter | Parser, resolver and evaluator for the same rules. The bootstrap is also a Lox++ program with three enums (`Expr`, `Stmt`, `Pattern`) and about 150 constructor-pattern arms; those are rewritten. |
| LSP | The missing-arms code action inserts `E.A` arms. Hover and go-to-definition on `E.A`. |
| tree-sitter grammar | `E.A` in patterns, and the removal of the comma chain. The queries and highlight tests. |
| Editor syntaxes | VS Code, TextMate bundle and Neovim: highlight `E.A`. |
| Programs | 62 files declare an enum: examples, translation probes, compile-error cases, print-line tests, and the bootstrap. About 440 constructor-pattern arms and their constructor calls change. A script does the rewrite, because each enum resolves every bare constructor name in its file. |
| Tools | Any tool that parses patterns or constructor names. The node plan lists them after a search. |

## Sequencing

The change is too large for one pull request, and native runs the bootstrap,
so the two cannot change at different times. Three steps keep each pull
request green:

1. **Add the qualified form.** Native, the bootstrap, the LSP and the editors
   accept `E.A` in expressions and patterns, next to the bare form. Old
   programs still run. The spec states the qualified form.
2. **Rewrite the programs.** The script rewrites every program to the
   qualified form. No behavior changes.
3. **Remove the bare form.** Constructors stop being globals. Shared names,
   the enum test, the field count rule, and the `or` grammar take effect.
   The four issues close here.

Step 1 alone is correct for every program that uses only one form, because
the qualified form resolves to the same constructor as the bare form. Step 3
exists to remove the bare form, so it is a separate node with its own probes.
The mission tracking issue #771 holds the DAG.

## Alternatives

- **Bare names when unique, `E.A` when not.** This also settles #691, #696
  and #747. It keeps the typing-error problem: `case Nnoe` stays a binding.
  It also makes a name that is valid today ambiguous when another file or a
  later import declares the same constructor, so a change in one place
  breaks a program in another place.
- **Keep the flat namespace.** State that constructor names are unique across
  all enums, and decide the four issues one by one. This is the cheapest
  change. It keeps the typing-error problem and the four separate rules.
- **Infer the enum from the subject**, as Swift's `.some` does. This needs a
  static type on the subject. Lox++ is dynamically typed
  (`docs/decisions/dynamic-functional-programming.md`), so it is not possible.

## The comma chain

Pattern rule 8 removes `case 1, 2 =>`. The comma chain is not a bug, and it
does not give a wrong result alone. The case for its removal is simplicity,
not safety.

### What the comma chain costs today

- **Two arm forms.** The grammar has a literal arm, joined by `,`, and an
  identifier arm, joined by `or`. Every parser must choose the form from the
  first token. Four parsers do this: native, the bootstrap, the tree-sitter
  grammar, and each editor syntax that highlights patterns.
- **The split caused #756.** Native parses a literal arm as an expression at
  `or` precedence (`src/compiler.cpp`, the literal branch of the arm parser),
  then compares it with the subject. So `case 1 or 2` compiles as
  `subject == (1 or 2)`, which is `subject == 1`. The bootstrap reads two
  literal alternatives. Each parser made a reasonable guess about a form that
  the grammar left out.
- **Two spellings for one idea.** "Match any of these alternatives" has one
  operator for literals and another for every other pattern. A program
  cannot write `case 0 or E.A =>`, because the two forms do not mix.
- **The spec keeps it "for backward compatibility".** `AGENTS.md` says that a
  language change rewrites every affected program, with no deprecation
  period. One program uses the comma chain (`examples/match_http_status.lox`,
  line 7).

### Options

| Option | `case 1, 2` | `case 1 or 2` | Arm forms | #756 |
|---|---|---|---|---|
| A. Remove the comma chain (chosen) | static error | matches 1 and 2 | one | fixed by the grammar |
| B. Keep the comma, reject `or` after a literal | matches 1 and 2 | static error | two | fixed by a parser check |
| C. Keep both | matches 1 and 2 | matches 1 and 2 | one, with two spellings | fixed by the grammar |
| D. Use `,` for every alternative | matches 1 and 2 | static error | one | fixed by the grammar |

- **A** gives one operator and one arm form. The cost is one rewritten line,
  and `or` is longer to type than `,`.
- **B** is the original deliverable of #756. It keeps every program valid,
  but it keeps the two arm forms and the first-token choice.
- **C** keeps two spellings that each parser and the spec must describe.
- **D** is not good with this proposal: `case E.A(x, y), E.B(x, y)` puts
  commas at two levels in one arm, and `or` already joins identifier
  alternatives in 10 programs.

The owner chose A on 2026-10-21.

## Relation to the import system

`docs/decisions/import-system.md` is accepted, and its mission has not
started. The two changes are independent: neither one blocks the other. This
proposal should land first, for three reasons:

- It is smaller, and it closes four open issues.
- It fixes the shape `name.member` in patterns before the import mission adds
  a second kind of name before the `.`.
- The bootstrap gets the new pattern forms while it is still one file. The
  import decision names the multi-file bootstrap as its largest per-backend
  cost (residual edge 2), so it is better not to do both at once.

### What the import mission inherits

- **One syntax for qualified names.** The import decision binds a module as
  an object, with members read through `.` (Axis 2). A constructor of an
  imported enum is then `shapes.Shape.Circle(1)`. The import mission extends
  `ctorPat` from `IDENTIFIER "." IDENTIFIER` to a path,
  `IDENTIFIER ( "." IDENTIFIER )+`. The last segment is the constructor, and
  the segment before it is the enum.
- **Static resolution.** Imports resolve at compile time and exports are
  explicit, so the compiler knows every enum that a module exports. A pattern
  such as `case shapes.Shape.Circle(r)` resolves statically, and the
  exhaustiveness check and the field-count rule work across modules with no
  run-time lookup. For the same reason `E` does not have to be a value
  (owner decision 3): the compiler resolves the whole path. The import
  mission decides if `shapes.Shape` alone is a value.
- **A short name.** Three segments are long. The import decision names
  selective import (`from m import f`) as an increment. If the mission adds
  it, `from shapes import Shape` binds the enum name, and a program writes
  `Shape.Circle` again. The mission chooses if it adds selective import.
- **Name rules.** Declaration rule 4 reserves an enum name against every
  other global name and every local. A module name must follow the same rule,
  so that the first segment of a path has one meaning.
- **Hidden globals.** Source inclusion composes all modules into one program.
  Two modules may each declare an enum `Shape`, so the hidden global of a
  constructor must include the module in its name, not only `Shape.Circle`.
  The tag of each constructor stays unique in the composed program, and the
  native limit of 256 constructors applies to the program with all its
  imports.
- **Combinator modules.** The import decision makes `Option` and `Result`
  modules. Under this proposal such a module exports the enum, and its
  functions use `Option.Some` inside the module, as every other program does.

## Non-goals

- **Bare class patterns.** A bare class name has the same typing-error
  problem as a bare constructor name. The fix is a separate change.
- **Methods on enums.** `docs/decisions/design-tensions-2026-10-08.md`
  decided on pipes and modules instead.
- **Import syntax.** How a module exports an enum, and how an importer names
  it, belongs to the import mission. "Relation to the import system" lists
  what that mission inherits.
