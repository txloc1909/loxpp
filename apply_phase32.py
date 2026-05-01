#!/usr/bin/env python3
"""
Apply all Phase 3.2 (match-as-expression) changes atomically.
This script modifies spec, design notes, compiler, and tests.
"""
import re
import sys

def apply_spec_02_syntax():
    """Update spec/02-syntax.md with matchExpr grammar."""
    with open('spec/02-syntax.md', 'r') as f:
        content = f.read()

    # Add matchExpr to primary rule
    old = '''primary        ::= "true"
                 | "false"
                 | "nil"
                 | NUMBER
                 | STRING
                 | IDENTIFIER
                 | "this"
                 | "super" "." IDENTIFIER
                 | "(" expression ")"
                 | "[" ( expression ( "," expression )* )? "]"
                 | "{" ( mapEntry ( "," mapEntry )* )? "}" ;

mapEntry       ::= expression ":" expression ;'''

    new = '''primary        ::= "true"
                 | "false"
                 | "nil"
                 | NUMBER
                 | STRING
                 | IDENTIFIER
                 | "this"
                 | "super" "." IDENTIFIER
                 | "(" expression ")"
                 | "[" ( expression ( "," expression )* )? "]"
                 | "{" ( mapEntry ( "," mapEntry )* )? "}"
                 | matchExpr ;

matchExpr      ::= "match" expression "{" matchArm* "}" ;

mapEntry       ::= expression ":" expression ;'''

    content = content.replace(old, new)

    # Update armBody rule
    old_body = '''armBody        ::= statement | "{" declaration* "}" ;'''
    new_body = '''armBody        ::= statement
                 | "{" declaration* "}"
                 | "{" declaration* expression "}"
                 | expression ;          (* last two forms: expression-mode arms only *)'''

    content = content.replace(old_body, new_body)

    with open('spec/02-syntax.md', 'w') as f:
        f.write(content)
    print("✓ spec/02-syntax.md")

def apply_spec_04_semantics():
    """Update spec/04-semantics.md with match expression semantics."""
    with open('spec/04-semantics.md', 'r') as f:
        content = f.read()

    match_expr_section = '''### match Expression

```
match subject { matchArm* }
```

1. Evaluate `subject` exactly once and store it internally (not user-accessible).
2. Arms are tested in source order. For each arm:
   a. Evaluate the pattern against the stored subject (tag check, literal equality, or wildcard).
   b. If the pattern does not match, proceed to the next arm.
   c. If the pattern matches and a guard (`if expr`) is present, evaluate the guard. If the guard is falsy, proceed to the next arm.
   d. If the pattern matches (and the guard, if present, is truthy), evaluate the arm's body **expression** and return its value as the result of the `match` expression.
3. If no arm matches: raise **`MatchError`** at runtime. The `match` expression does not produce a value.

All arms must be written so that their body expressions leave exactly one value; this is enforced structurally by the compiler (each arm stores its result into a pre-allocated slot before cleanup).

When a `match` expression is used as a statement (`exprStmt`), the result value is discarded.

'''

    # Insert after Function Call section
    old_insert = "Functions may be called recursively. The depth limit is implementation-defined;\nexceeding it is a **runtime error**.\n\n---\n\n## Statements"
    new_insert = f"Functions may be called recursively. The depth limit is implementation-defined;\nexceeding it is a **runtime error**.\n\n{match_expr_section}---\n\n## Statements"

    content = content.replace(old_insert, new_insert)

    # Update Runtime Errors table
    old_error = "| No arm matches in a `match` statement | `match 99 { case 1 => ... }` |"
    new_error = "| No arm matches in a `match` statement or expression | `match 99 { case 1 => \"one\" }` |"
    content = content.replace(old_error, new_error)

    with open('spec/04-semantics.md', 'w') as f:
        f.write(content)
    print("✓ spec/04-semantics.md")

def apply_notes_pattern_matching():
    """Update notes/pattern-matching.md with Phase 3.2."""
    with open('notes/pattern-matching.md', 'r') as f:
        content = f.read()

    phase_32 = '''---

### Phase 3.2 — Match as expression (planned)

Promotion of `match` from a statement to a primary expression, enabling its use on the RHS of assignments, in function arguments, and as nested expressions. Independent of Phase 2.5 and can ship in parallel.

**Syntax:**

```lox
var area = match shape {
  case Circle(r)  => 3.14159 * r * r
  case Rect(w, h) => w * h
};

var result = match value {
  case Ok(v) if v > 0 => { var x = v * 2; x }
  case Ok(v)          => v
  case Err(m)         => 0
};
```

**Grammar:** `match` joins `primary` as a `matchExpr` alternative. Each `armBody` can be a bare expression or `{ decl* expression }` (last item without `;` is the value). In statement position, the form is identical to Phase 1 (single statement or braced block); in expression position, only expression-producing forms are valid.

**Semantics:** Evaluate subject once. For each arm, check pattern (tag check, equality, wildcard). On match, evaluate the arm's body expression and return its value. If no arm matches and no catch-all, raise `MatchError`.

**Stack guarantee:** Each arm stores its result into a pre-allocated hidden slot before cleanup. All arms leave exactly one value via this mechanism — the compiler enforces structural uniformity, no runtime delta checking needed.

**Statement use:** When used as a statement (e.g., `match x { case 1 => f(); }` with a lone bare statement arm in statement context, or `match x { ... };` in `exprStmt`), the result is `POP`ped by the normal expression statement path. Stack-neutral.

**No new tokens, no new opcodes:** Uses existing `SET_LOCAL` (peek, no pop) and `POP`. No changes to `MATCH_ERROR` (already aborts, never returns).

'''

    # Insert before Phase 2.5
    old_insert = "---\n\n### Phase 2.5 — Class patterns (planned)"
    new_insert = phase_32 + old_insert
    content = content.replace(old_insert, new_insert)

    # Update dependency chain
    old_chain = '''Phase 1.5: var {x, y} destructuring                  ✅ PR #62
  ↓
Phase 2:   enum + constructor pattern + exhaustiveness ✅ PR #64
  ↓
Phase 2.5: class patterns (instanceof dispatch, named-field binding)
  ↓
Phase 3:   or-patterns, match-as-expression, var [a,b]
  ↓
Phase 4:   list patterns, @-bindings, not-patterns
  ↓
Phase 5:   JUMP_TABLE optimization'''

    new_chain = '''Phase 1.5: var {x, y} destructuring                  ✅ PR #62
  ↓
Phase 2:   enum + constructor pattern + exhaustiveness ✅ PR #64
  ↓                                    ↓
Phase 2.5: class patterns          Phase 3.2: match-as-expression (planned)
  ↓                                    ↓
Phase 3:   or-patterns, var [a,b]  ←┘
  ↓
Phase 4:   list patterns, @-bindings, not-patterns
  ↓
Phase 5:   JUMP_TABLE optimization'''

    content = content.replace(old_chain, new_chain)

    with open('notes/pattern-matching.md', 'w') as f:
        f.write(content)
    print("✓ notes/pattern-matching.md")

def apply_parser_cpp():
    """Update src/parser.cpp to register matchExpression."""
    with open('src/parser.cpp', 'r') as f:
        content = f.read()

    old = "    RULE(MATCH,          nullptr,             nullptr,           NONE),"
    new = "    RULE(MATCH,          &Compiler::matchExpression, nullptr,      NONE),"

    content = content.replace(old, new)

    with open('src/parser.cpp', 'w') as f:
        f.write(content)
    print("✓ src/parser.cpp")

def apply_compiler_h():
    """Update src/compiler.h with method declarations."""
    with open('src/compiler.h', 'r') as f:
        content = f.read()

    # Add matchExpression declaration
    old = '''    void matchStatement();
    void enumDeclaration();'''
    new = '''    void matchStatement();
    void matchExpression();
    void enumDeclaration();'''
    content = content.replace(old, new)

    # Update compileMatchArm signature
    old_sig = '''    MatchArmResult compileMatchArm(int subjectSlot, int armLocalBase);'''
    new_sig = '''    MatchArmResult compileMatchArm(int subjectSlot, int armLocalBase, int resultSlot, bool asExpr);'''
    content = content.replace(old_sig, new_sig)

    # Add compileMatchBody private method
    old_private = '''    void varDestructure();
    void emitDestructureLocal(const std::vector<Token>& fields);
    void emitDestructureGlobal(const std::vector<Token>& fields);

    void parseFunction(FunctionType type);'''
    new_private = '''    void varDestructure();
    void emitDestructureLocal(const std::vector<Token>& fields);
    void emitDestructureGlobal(const std::vector<Token>& fields);

    void compileMatchBody(bool asExpr);
    void parseFunction(FunctionType type);'''
    content = content.replace(old_private, new_private)

    with open('src/compiler.h', 'w') as f:
        f.write(content)
    print("✓ src/compiler.h")

def apply_compiler_cpp():
    """Update src/compiler.cpp with implementation."""
    with open('src/compiler.cpp', 'r') as f:
        content = f.read()

    # Replace matchStatement with thin wrapper + add matchExpression and compileMatchBody
    old_match_stmt = '''void Compiler::matchStatement() {
    beginScope();

    // Compile subject into a hidden local so case arms can reload it with
    // GET_LOCAL.
    expression();
    Token syntheticName{TokenType::IDENTIFIER, "(match)",
                        m_parser->m_previous.line};
    addLocal(syntheticName);
    markInitialized();
    int subjectSlot = m_localCount - 1;

    m_parser->consume(TokenType::LEFT_BRACE, "Expect '{' before match body.");

    // start = -1 flags this context as a match (not a loop).
    m_loopStack.push_back({-1, m_localCount, {}});

    bool hasUnguardedCatchAll = false;
    std::set<std::string> seenCtors;

    while (!m_parser->check(TokenType::RIGHT_BRACE) &&
           !m_parser->check(TokenType::EOF_)) {
        if (!m_parser->match(TokenType::CASE)) {
            m_parser->error("Expect 'case' in match body.");
            break;
        }
        if (hasUnguardedCatchAll) {
            m_parser->error("Unreachable arm after catch-all.");
        }

        int armLocalBase = m_localCount;
        auto arm = compileMatchArm(subjectSlot, armLocalBase);
        if (arm.isUnguardedCatchAll) {
            hasUnguardedCatchAll = true;
        }
        if (!arm.ctorName.empty()) {
            seenCtors.insert(arm.ctorName);
        }

        // Guard-fail exit: pop guard bool, pop binding locals.
        // If lastMiss is also set (constructor arm), the guard-fail path must
        // jump over the lastMiss POP to avoid a spurious extra pop.
        int skipLastMissPop = -1;
        if (arm.guardMiss != -1) {
            patchJump(arm.guardMiss);
            emitByte(Op::POP); // pop false guard
            for (int i = 0; i < arm.bindingCount; i++) {
                emitByte(Op::POP); // pop binding value(s)
            }
            if (arm.lastMiss != -1) {
                skipLastMissPop = emitJump(Op::JUMP);
            }
        }

        // Literal/tag miss: pop false equality, fall through to next arm.
        if (arm.lastMiss != -1) {
            patchJump(arm.lastMiss);
            emitByte(Op::POP);
        }

        if (skipLastMissPop != -1) {
            patchJump(skipLastMissPop);
        }
    }

    m_parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after match body.");

    if (!hasUnguardedCatchAll && !seenCtors.empty()) {
        checkEnumExhaustiveness(seenCtors);
    }

    // No unguarded catch-all → raise MatchError when no arm matched.
    if (!hasUnguardedCatchAll) {
        emitByte(Op::MATCH_ERROR);
    }

    for (int j : m_loopStack.back().breakJumps) {
        patchJump(j);
    }
    m_loopStack.pop_back();
    endScope(); // pops the hidden subject local
}'''

    new_match_impl = '''void Compiler::matchExpression() {
    compileMatchBody(/*asExpr=*/true);
}

void Compiler::matchStatement() {
    compileMatchBody(/*asExpr=*/false);
}

void Compiler::compileMatchBody(bool asExpr) {
    beginScope();

    // Compile subject into a hidden local so case arms can reload it with
    // GET_LOCAL.
    expression();
    Token syntheticName{TokenType::IDENTIFIER, "(match)",
                        m_parser->m_previous.line};
    addLocal(syntheticName);
    markInitialized();
    int subjectSlot = m_localCount - 1;

    // In expression mode, pre-allocate a result slot to hold each arm's value.
    int resultSlot = -1;
    if (asExpr) {
        emitByte(Op::NIL);
        Token resultName{TokenType::IDENTIFIER, "(match_result)",
                         m_parser->m_previous.line};
        addLocal(resultName);
        markInitialized();
        resultSlot = m_localCount - 1;
    }

    int armLocalBase = m_localCount;

    m_parser->consume(TokenType::LEFT_BRACE, "Expect '{' before match body.");

    // start = -1 flags this context as a match (not a loop).
    m_loopStack.push_back({-1, m_localCount, {}});

    bool hasUnguardedCatchAll = false;
    std::set<std::string> seenCtors;

    while (!m_parser->check(TokenType::RIGHT_BRACE) &&
           !m_parser->check(TokenType::EOF_)) {
        if (!m_parser->match(TokenType::CASE)) {
            m_parser->error("Expect 'case' in match body.");
            break;
        }
        if (hasUnguardedCatchAll) {
            m_parser->error("Unreachable arm after catch-all.");
        }

        int armLocalBase_iter = m_localCount;
        auto arm = compileMatchArm(subjectSlot, armLocalBase_iter, resultSlot, asExpr);
        if (arm.isUnguardedCatchAll) {
            hasUnguardedCatchAll = true;
        }
        if (!arm.ctorName.empty()) {
            seenCtors.insert(arm.ctorName);
        }

        // Guard-fail exit: pop guard bool, pop binding locals.
        // If lastMiss is also set (constructor arm), the guard-fail path must
        // jump over the lastMiss POP to avoid a spurious extra pop.
        int skipLastMissPop = -1;
        if (arm.guardMiss != -1) {
            patchJump(arm.guardMiss);
            emitByte(Op::POP); // pop false guard
            for (int i = 0; i < arm.bindingCount; i++) {
                emitByte(Op::POP); // pop binding value(s)
            }
            if (arm.lastMiss != -1) {
                skipLastMissPop = emitJump(Op::JUMP);
            }
        }

        // Literal/tag miss: pop false equality, fall through to next arm.
        if (arm.lastMiss != -1) {
            patchJump(arm.lastMiss);
            emitByte(Op::POP);
        }

        if (skipLastMissPop != -1) {
            patchJump(skipLastMissPop);
        }
    }

    m_parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after match body.");

    if (!hasUnguardedCatchAll && !seenCtors.empty()) {
        checkEnumExhaustiveness(seenCtors);
    }

    // No unguarded catch-all → raise MatchError when no arm matched.
    if (!hasUnguardedCatchAll) {
        emitByte(Op::MATCH_ERROR);
    }

    for (int j : m_loopStack.back().breakJumps) {
        patchJump(j);
    }
    m_loopStack.pop_back();

    if (!asExpr) {
        endScope(); // pops the hidden subject local
    } else {
        // Stack: [..., subject, result_placeholder(holds winning arm value)]
        // Move result into subject slot and pop the redundant top copy.
        emitBytes(Op::SET_LOCAL, static_cast<uint8_t>(subjectSlot));
        emitByte(Op::POP);
        // Stack: [..., result]   (at subject's old slot position)
        // Manually close scope without emitting POP for the result.
        m_scopeDepth--;
        m_localCount = subjectSlot;
    }
}'''

    content = content.replace(old_match_stmt, new_match_impl)

    # Update compileMatchArm signature and implementation
    old_sig = "Compiler::MatchArmResult Compiler::compileMatchArm(int subjectSlot,\n                                                   int armLocalBase) {"
    new_sig = "Compiler::MatchArmResult Compiler::compileMatchArm(int subjectSlot,\n                                                   int armLocalBase,\n                                                   int resultSlot,\n                                                   bool asExpr) {"
    content = content.replace(old_sig, new_sig)

    # Update body section in compileMatchArm
    old_body = '''    m_parser->consume(TokenType::FAT_ARROW, "Expect '=>' after pattern.");

    // Body (single statement or braced block).
    beginScope();
    if (m_parser->match(TokenType::LEFT_BRACE)) {
        while (!m_parser->check(TokenType::RIGHT_BRACE) &&
               !m_parser->check(TokenType::EOF_)) {
            declaration();
        }
        m_parser->consume(TokenType::RIGHT_BRACE,
                          "Expect '}' after match arm body.");
    } else {
        statement();
    }
    endScope();'''

    new_body = '''    m_parser->consume(TokenType::FAT_ARROW, "Expect '=>' after pattern.");

    // Body (single statement or braced block, or expression-mode arm).
    beginScope();
    if (asExpr) {
        // Expression mode: arm body must produce a value.
        // { decls* expr }  — final expression (no trailing ';') is the value.
        // or bare expr.
        if (m_parser->match(TokenType::LEFT_BRACE)) {
            while (!m_parser->check(TokenType::RIGHT_BRACE) &&
                   !m_parser->check(TokenType::EOF_)) {
                if (m_parser->check(TokenType::VAR) ||
                    m_parser->check(TokenType::FUN) ||
                    m_parser->check(TokenType::CLASS) ||
                    m_parser->check(TokenType::ENUM)) {
                    declaration();
                } else {
                    expression();
                    if (m_parser->match(TokenType::SEMICOLON)) {
                        emitByte(Op::POP);  // discard intermediate expression
                    } else {
                        break;  // last expression — the arm value
                    }
                }
            }
            m_parser->consume(TokenType::RIGHT_BRACE,
                              "Expect '}' after match arm body.");
        } else {
            expression();
        }
        // Save arm result into pre-allocated result slot, pop redundant copy.
        emitBytes(Op::SET_LOCAL, static_cast<uint8_t>(resultSlot));
        emitByte(Op::POP);
    } else {
        // Statement mode: original behavior (single statement or braced block).
        if (m_parser->match(TokenType::LEFT_BRACE)) {
            while (!m_parser->check(TokenType::RIGHT_BRACE) &&
                   !m_parser->check(TokenType::EOF_)) {
                declaration();
            }
            m_parser->consume(TokenType::RIGHT_BRACE,
                              "Expect '}' after match arm body.");
        } else {
            statement();
        }
    }
    endScope();'''

    content = content.replace(old_body, new_body)

    with open('src/compiler.cpp', 'w') as f:
        f.write(content)
    print("✓ src/compiler.cpp")

def apply_test_cmakelists():
    """Update test/CMakeLists.txt to register test_match_expr."""
    with open('test/CMakeLists.txt', 'r') as f:
        content = f.read()

    # Add test_match_expr executable
    old = '''add_executable(test_enum test_enum.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_enum_gc test_enum.cpp test_harness.cpp ${FULL_SRCS})

target_include_directories(test_main PRIVATE ${PROJECT_SOURCE_DIR}/src)'''

    new = '''add_executable(test_enum test_enum.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_enum_gc test_enum.cpp test_harness.cpp ${FULL_SRCS})
add_executable(test_match_expr test_match_expr.cpp test_harness.cpp ${FULL_SRCS})

target_include_directories(test_main PRIVATE ${PROJECT_SOURCE_DIR}/src)'''

    content = content.replace(old, new)

    # Add target_include_directories for test_match_expr
    old_incl = '''target_include_directories(test_enum PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_enum_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_definitions(test_stress_gc PRIVATE LOXPP_STRESS_GC)'''

    new_incl = '''target_include_directories(test_enum PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_enum_gc PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_match_expr PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_definitions(test_stress_gc PRIVATE LOXPP_STRESS_GC)'''

    content = content.replace(old_incl, new_incl)

    # Add target_link_libraries for test_match_expr
    old_link = '''target_link_libraries(test_enum PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_enum_gc PRIVATE GTest::GTest GTest::Main)

add_test(NAME TestMain COMMAND test_main)'''

    new_link = '''target_link_libraries(test_enum PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_enum_gc PRIVATE GTest::GTest GTest::Main)
target_link_libraries(test_match_expr PRIVATE GTest::GTest GTest::Main)

add_test(NAME TestMain COMMAND test_main)'''

    content = content.replace(old_link, new_link)

    # Add add_test for test_match_expr
    old_test = '''add_test(NAME TestEnum COMMAND test_enum)
add_test(NAME TestEnumGC COMMAND test_enum_gc)'''

    new_test = '''add_test(NAME TestEnum COMMAND test_enum)
add_test(NAME TestEnumGC COMMAND test_enum_gc)
add_test(NAME TestMatchExpr COMMAND test_match_expr)'''

    content = content.replace(old_test, new_test)

    with open('test/CMakeLists.txt', 'w') as f:
        f.write(content)
    print("✓ test/CMakeLists.txt")

def create_test_file():
    """Create test/test_match_expr.cpp."""
    test_code = '''#include "test_harness.h"
#include "function.h"
#include "value.h"
#include <gtest/gtest.h>

static void expect_num(Value v, double n) {
    ASSERT_TRUE(is<double>(v));
    EXPECT_EQ(as<double>(v), n);
}

static void expect_str(Value v, const char* s) {
    ASSERT_TRUE(is<Obj*>(v));
    ASSERT_TRUE(isObjType(as<Obj*>(v), ObjType::STRING));
    EXPECT_EQ(asString(as<Obj*>(v))->data, std::string(s));
}

class MatchExpressionTest : public ::testing::Test {};

TEST_F(MatchExpressionTest, LiteralArm) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match 1 {
            case 1 => 42
            case _ => 0
        };
    )"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42);
}

TEST_F(MatchExpressionTest, EnumConstructor) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(value) Err(msg) }
        var x = match Ok(5) {
            case Ok(v) => v
            case Err(m) => 0
        };
    )"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 5);
}

TEST_F(MatchExpressionTest, GuardPass) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(value) Err(msg) }
        var x = match Ok(5) {
            case Ok(v) if v > 3 => 1
            case Ok(v) => 2
            case Err(m) => 3
        };
    )"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(MatchExpressionTest, BracedBody) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match 1 {
            case 1 => { var t = 10; t * 2 }
            case _ => 0
        };
    )"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 20);
}

TEST_F(MatchExpressionTest, StackNeutral) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 42;
        match x {
            case 42 => 100
            case _ => 200
        };
        var y = 99;
    )"), InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(MatchExpressionTest, MatchInFunctionReturn) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f(v) {
            return match v {
                case 1 => "one"
                case 2 => "two"
                case _ => "other"
            };
        }
        var x = f(1);
    )"), InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_str(*v, "one");
}

TEST_F(MatchExpressionTest, NoMatchRaisesError) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match 5 {
            case 1 => 10
        };
    )"), InterpretResult::RUNTIME_ERROR);
}
'''

    with open('test/test_match_expr.cpp', 'w') as f:
        f.write(test_code)
    print("✓ test/test_match_expr.cpp")

def create_example_file():
    """Create examples/enum_match.lox."""
    example = '''// enum_match.lox — Match as expression with algebraic types.
//
// Demonstrates: enum, match-as-expression, guard clauses, nested match.

enum Result { Ok(value) Err(msg) }
enum Option { Some(v) None }

fun divide(a, b) {
    if (b == 0) return Err("division by zero");
    return Ok(a / b);
}

fun safeSqrt(n) {
    if (n < 0) return None();
    return Some(math.sqrt(n));
}

// CHECK: === Result / Option demo ===
print "=== Result / Option demo ===";

var cases = [divide(10, 2), divide(7, 0), divide(9, 3)];
var i = 0;
while (i < 3) {
    var label = match cases[i] {
        case Ok(v)  => "ok: " + str(v)
        case Err(m) => "err: " + m
    };
    print label;
    i = i + 1;
}
// CHECK: ok: 5
// CHECK: err: division by zero
// CHECK: ok: 3

print "";
var roots = [safeSqrt(4), safeSqrt(-1), safeSqrt(9)];
i = 0;
while (i < 3) {
    print match roots[i] {
        case Some(v) => "sqrt = " + str(v)
        case None    => "no real root"
    };
    i = i + 1;
}
// CHECK: sqrt = 2
// CHECK: no real root
// CHECK: sqrt = 3

// Nested match as expression.
print "";
var r = Ok(42);
var desc = match r {
    case Ok(v) => match v {
        case 0  => "zero"
        case 42 => "the answer"
        case _  => "other"
    }
    case Err(m) => "error"
};
print desc;
// CHECK: the answer
'''

    with open('examples/enum_match.lox', 'w') as f:
        f.write(example)
    print("✓ examples/enum_match.lox")

def main():
    """Apply all Phase 3.2 changes."""
    try:
        print("Applying Phase 3.2: Match as Expression")
        print("=" * 50)
        apply_spec_02_syntax()
        apply_spec_04_semantics()
        apply_notes_pattern_matching()
        apply_parser_cpp()
        apply_compiler_h()
        apply_compiler_cpp()
        apply_test_cmakelists()
        create_test_file()
        create_example_file()
        print("=" * 50)
        print("✅ All changes applied successfully!")
        return 0
    except Exception as e:
        print(f"❌ Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1

if __name__ == '__main__':
    sys.exit(main())
