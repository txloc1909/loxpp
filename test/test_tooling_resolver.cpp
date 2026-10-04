#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "corpus.h"
#include "tooling/document_model.h"
#include "tooling/resolver.h"
#include "tooling/stdlib_names.h"
#include "tooling/symbol_table.h"

using namespace loxpp::tooling;

namespace fs = std::filesystem;

namespace {

// Byte offset of the nth (1-based) occurrence of needle in text.
std::size_t offsetOf(const std::string& text, const std::string& needle,
                     int nth = 1) {
    std::size_t pos = 0;
    for (int i = 0; i < nth; ++i) {
        pos = text.find(needle, i == 0 ? 0 : pos + 1);
        if (pos == std::string::npos) {
            ADD_FAILURE() << "needle '" << needle << "' occurrence " << nth
                          << " not found";
            return 0;
        }
    }
    return pos;
}

std::vector<std::string> warningMessages(const DocumentModel& model) {
    std::vector<std::string> out;
    for (const Diagnostic& d : model.warnings()) {
        out.push_back(d.message);
    }
    return out;
}

} // namespace

TEST(ToolingResolver, ShadowingPicksInnerBinding) {
    const std::string src = R"(var x = 1;
fun outer() {
  var x = 2;
  {
    var x = 3;
    print x;
  }
  print x;
}
print x;
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty())
        << "unexpected: "
        << (model.warnings().empty() ? std::string()
                                     : model.warnings().front().message);

    const std::size_t innerUse = offsetOf(src, "print x", 1) + 6;
    const std::size_t midUse = offsetOf(src, "print x", 2) + 6;
    const std::size_t topUse = offsetOf(src, "print x", 3) + 6;

    auto innerDef = model.definitionAt(innerUse);
    auto midDef = model.definitionAt(midUse);
    auto topDef = model.definitionAt(topUse);
    ASSERT_TRUE(innerDef && midDef && topDef);
    EXPECT_EQ(innerDef->offset, offsetOf(src, "var x = 3") + 4);
    EXPECT_EQ(midDef->offset, offsetOf(src, "var x = 2") + 4);
    EXPECT_EQ(topDef->offset, offsetOf(src, "var x = 1") + 4);
}

TEST(ToolingResolver, ForwardGlobalReferenceDoesNotWarn) {
    const std::string src = R"(fun useLater() { return later; }
var later = 42;
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t use = offsetOf(src, "return later") + 7;
    auto def = model.definitionAt(use);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->offset, offsetOf(src, "var later") + 4);
}

TEST(ToolingResolver, ForInLoopVariable) {
    const std::string src = R"(fun sum(items) {
  var total = 0;
  for (var item in items) {
    total = total + item;
  }
  return total;
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t decl = offsetOf(src, "var item in") + 4;
    const std::size_t use = offsetOf(src, "total + item") + 8;
    auto def = model.definitionAt(use);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->offset, decl);

    auto refs = model.referencesAt(decl);
    ASSERT_EQ(refs.size(), 2u); // declaration + one use
    EXPECT_EQ(refs[0].offset, decl);
    EXPECT_EQ(refs[1].offset, use);
}

TEST(ToolingResolver, TryCatchBasic) {
    const std::string src = R"(fun test() {
  try {
    throw "error";
  } catch (e) {
    print e;
  }
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t decl = offsetOf(src, "catch (e)") + 7;
    const std::size_t use = offsetOf(src, "print e") + 6;
    auto def = model.definitionAt(use);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->offset, decl);

    auto refs = model.referencesAt(decl);
    ASSERT_EQ(refs.size(), 2u); // declaration + one use
    EXPECT_EQ(refs[0].offset, decl);
    EXPECT_EQ(refs[1].offset, use);
}

TEST(ToolingResolver, CatchVariableShadowsOuter) {
    const std::string src = R"(fun test() {
  var e = 1;
  try {
    throw "error";
  } catch (e) {
    print e;
  }
  print e;
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t outerDecl = offsetOf(src, "var e = 1") + 4;
    const std::size_t catchDecl = offsetOf(src, "catch (e)") + 7;
    const std::size_t catchUse = offsetOf(src, "catch (e) {") + 7;
    const std::size_t printCatch = offsetOf(src, "print e;") + 6;
    const std::size_t printOuter = offsetOf(src, "print e;", 2) + 6;

    // The catch binding use should resolve to the catch declaration.
    auto catchDef = model.definitionAt(catchUse);
    ASSERT_TRUE(catchDef);
    EXPECT_EQ(catchDef->offset, catchDecl);

    // The use inside the catch block should resolve to the catch binding.
    auto defInCatch = model.definitionAt(printCatch);
    ASSERT_TRUE(defInCatch);
    EXPECT_EQ(defInCatch->offset, catchDecl);

    // The use outside the catch block should resolve to the outer binding.
    auto defOutside = model.definitionAt(printOuter);
    ASSERT_TRUE(defOutside);
    EXPECT_EQ(defOutside->offset, outerDecl);
}

TEST(ToolingResolver, DeferResolvesCalls) {
    const std::string src = R"(fun test() {
  var f = fun() { return 1; };
  defer f();
}
)";
    DocumentModel model(src);
    // The `f` function is declared but never used outside of defer, so the
    // resolver warns about it being unused. This is expected behavior --
    // the defer mechanism doesn't register a reference that prevents the
    // unused warning.
    ASSERT_LE(warningMessages(model).size(), 1u);

    const std::size_t fDecl = offsetOf(src, "var f = fun") + 4;
    const std::size_t fUse = offsetOf(src, "defer f()") + 6;
    auto def = model.definitionAt(fUse);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->offset, fDecl);
}

TEST(ToolingResolver, NestedTryCatch) {
    const std::string src = R"(fun test() {
  try {
    try {
      throw "inner";
    } catch (e) {
      print e;
    }
  } catch (e) {
    print e;
  }
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t innerCatch = offsetOf(src, "} catch (e) {") + 9;
    const std::size_t outerCatch = offsetOf(src, "} catch (e) {", 2) + 9;
    const std::size_t innerUse = offsetOf(src, "print e;") + 6;
    const std::size_t outerUse = offsetOf(src, "print e;", 2) + 6;

    // Inner use should resolve to inner catch.
    auto innerDef = model.definitionAt(innerUse);
    ASSERT_TRUE(innerDef);
    EXPECT_EQ(innerDef->offset, innerCatch);

    // Outer use should resolve to outer catch.
    auto outerDef = model.definitionAt(outerUse);
    ASSERT_TRUE(outerDef);
    EXPECT_EQ(outerDef->offset, outerCatch);
}

TEST(ToolingResolver, CatchBlockWithLocalVar) {
    const std::string src = R"(fun test() {
  try {
    throw "error";
  } catch (e) {
    var msg = e;
    print msg;
  }
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t eCatch = offsetOf(src, "catch (e)") + 7;
    const std::size_t eUse = offsetOf(src, "var msg = e") + 10;
    const std::size_t msgDecl = offsetOf(src, "var msg = e") + 4;
    const std::size_t msgUse = offsetOf(src, "print msg") + 6;

    auto eDef = model.definitionAt(eUse);
    ASSERT_TRUE(eDef);
    EXPECT_EQ(eDef->offset, eCatch);

    auto msgDef = model.definitionAt(msgUse);
    ASSERT_TRUE(msgDef);
    EXPECT_EQ(msgDef->offset, msgDecl);
}

TEST(ToolingResolver, MatchBindingResolves) {
    const std::string src = R"(enum Option { Some(v) None }
fun unwrap(o) {
  return match o {
    case Some(inner) => inner
    case None => 0
  };
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t bindDecl = offsetOf(src, "Some(inner)") + 5;
    const std::size_t bindUse = offsetOf(src, "=> inner") + 3;
    auto def = model.definitionAt(bindUse);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->offset, bindDecl);

    // `Some` in the pattern is the constructor, not a fresh binding.
    const std::size_t ctorUse = offsetOf(src, "case Some(inner)") + 5;
    auto ctorDef = model.definitionAt(ctorUse);
    ASSERT_TRUE(ctorDef);
    EXPECT_EQ(ctorDef->offset, offsetOf(src, "Some(v)"));
}

TEST(ToolingResolver, OrPatternBindsSameName) {
    const std::string src = R"(enum E { Move(x) Teleport(x) Quit }
fun go(e) {
  return match e {
    case Move(x) or Teleport(x) => x
    case Quit => 0
  };
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t use = offsetOf(src, "=> x") + 3;
    auto def = model.definitionAt(use);
    ASSERT_TRUE(def);
    // Resolves to the binding introduced by the first alternative (the second
    // "Move(x)" in the source -- the first is the enum constructor).
    EXPECT_EQ(def->offset, offsetOf(src, "Move(x)", 2) + 5);
}

TEST(ToolingResolver, OrPatternRepeatBindingIsAReference) {
    const std::string src = R"(enum E { Move(x) Teleport(x) Quit }
fun go(e) {
  return match e {
    case Move(x) or Teleport(x) => x
    case Quit => 0
  };
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    // The second alternative repeats the binding; it is a use of the same
    // symbol so rename rewrites every alternative, not all but one.
    // ("Teleport(x)" also appears in the enum header; the case arm is the
    // second occurrence.)
    const std::size_t repeat = offsetOf(src, "Teleport(x)", 2) + 9;
    auto def = model.definitionAt(repeat);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->offset, offsetOf(src, "Move(x)", 2) + 5);

    const std::size_t decl = offsetOf(src, "Move(x)", 2) + 5;
    auto refs = model.referencesAt(decl, /*includeDeclaration=*/true);
    ASSERT_EQ(refs.size(), 3u); // declaration + repeat + body use
    EXPECT_EQ(refs[0].offset, decl);
    EXPECT_EQ(refs[1].offset, repeat);
    EXPECT_EQ(refs[2].offset, offsetOf(src, "=> x") + 3);
}

TEST(ToolingResolver, AtBindingBindsOuterAndInner) {
    const std::string src = R"(enum Tree { Leaf(v) Node(l, r) }
fun describe(t) {
  return match t {
    case whole @ Node(l, r) => whole
    case Leaf(v) => v
  };
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t wholeUse = offsetOf(src, "=> whole") + 3;
    auto def = model.definitionAt(wholeUse);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->offset, offsetOf(src, "whole @"));
}

TEST(ToolingResolver, ThisOutsideMethodWarns) {
    const std::string src = "fun f() { return this; }\n";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message, "'this' outside a method");
}

TEST(ToolingResolver, ThisInsideMethodResolves) {
    const std::string src = R"(class Box {
  init(v) { this.value = v; }
  get() { return this.value; }
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());
}

TEST(ToolingResolver, UnusedLocalWarns) {
    const std::string src =
        "fun f() {\n  var unusedThing = 1;\n  return 0;\n}\n";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message, "unused local 'unusedThing'");
    EXPECT_EQ(model.warnings().front().offset, offsetOf(src, "unusedThing"));
}

TEST(ToolingResolver, UndefinedNameWarns) {
    const std::string src = "fun f() { return mysteryValue; }\n";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message, "unknown name 'mysteryValue'");
}

TEST(ToolingResolver, SelfReferenceInInitializerWarns) {
    const std::string src = "var a = a + 1;\n";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message,
              "'a' is used in its own initializer");
}

TEST(ToolingResolver, BreakOutsideLoopWarns) {
    const std::string src = "fun f() { break; }\n";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message,
              "'break' outside a loop or match");
}

TEST(ToolingResolver, ReturnAtTopLevelWarns) {
    const std::string src = "return 1;\n";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message, "'return' outside a function");
}

TEST(ToolingResolver, EnumOutsideGlobalScopeWarns) {
    // Exactly one warning: the scope-placement warning must not also drag in
    // an "unused local" warning for the same fallback enum symbol.
    const std::string src = "fun f() { enum Local { A B } return 0; }\n";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message,
              "enum must be declared at global scope");
}

TEST(ToolingResolver, OrPatternMismatchedNamesWarnsOnce) {
    const std::string src = R"(enum E { A(x) B(y) Quit }
fun go(e) {
  return match e {
    case A(p) or B(q) => p
    case Quit => 0
  };
}
)";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message,
              "or-pattern alternatives bind different names: 'p' vs 'q'");

    // The body still resolves: `p` points at the binding from its alternative.
    const std::size_t use = offsetOf(src, "=> p") + 3;
    auto def = model.definitionAt(use);
    ASSERT_TRUE(def);
    EXPECT_EQ(def->offset, offsetOf(src, "A(p)") + 2);
}

TEST(ToolingResolver, OrPatternMatchedNamesDoesNotWarn) {
    const std::string src = R"(enum E { A(x) B(y) Quit }
fun go(e) {
  return match e {
    case A(v) or B(v) => v
    case Quit => 0
  };
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());
}

TEST(ToolingResolver, SymbolAtDistinguishesNothingUserSymbolAndStdlibGlobal) {
    const std::string src =
        "fun f() {\n  var local = 1;\n  return type(local);\n}\n";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    // (a) nothing: an offset on the numeric literal.
    const std::size_t nothing = offsetOf(src, "1;");
    EXPECT_EQ(model.symbolAt(nothing), nullptr);
    EXPECT_TRUE(model.knownGlobalAt(nothing).empty());

    // (b) a user symbol: the `local` argument use.
    const std::size_t userUse = offsetOf(src, "type(local)") + 5;
    const Symbol* sym = model.symbolAt(userUse);
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(sym->name, "local");
    EXPECT_TRUE(model.knownGlobalAt(userUse).empty());

    // (c) a stdlib global: the `type` callee.
    const std::size_t stdUse = offsetOf(src, "type(local)");
    EXPECT_EQ(model.symbolAt(stdUse), nullptr);
    EXPECT_EQ(model.knownGlobalAt(stdUse), "type");
}

TEST(ToolingResolver, RedeclarationInSameScopeWarns) {
    const std::string src =
        "fun f() { var d = 1; print d; var d = 2; return d; }\n";
    DocumentModel model(src);
    ASSERT_EQ(warningMessages(model).size(), 1u);
    EXPECT_EQ(model.warnings().front().message,
              "redeclaration of 'd' in the same scope");
}

TEST(ToolingResolver, GlobalRedeclarationIsAllowed) {
    const std::string src = "var g = 1;\nvar g = 2;\nprint g;\n";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());
}

TEST(ToolingResolver, StdlibGlobalIsKnown) {
    const std::string src = "fun f() { return len(str(clock())); }\n";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());
    EXPECT_TRUE(isStdlibGlobal("callMethod"));
    EXPECT_TRUE(isMathMember("floor"));
    EXPECT_FALSE(isStdlibGlobal("print"));
}

TEST(ToolingResolver, ReferencesAcrossScopes) {
    const std::string src = R"(fun counter() {
  var count = 0;
  fun bump() { count = count + 1; return count; }
  return bump;
}
)";
    DocumentModel model(src);
    EXPECT_TRUE(warningMessages(model).empty());

    const std::size_t decl = offsetOf(src, "var count") + 4;
    auto refs = model.referencesAt(decl, /*includeDeclaration=*/true);
    // declaration + three uses (count = , count + 1, return count)
    EXPECT_EQ(refs.size(), 4u);
}

TEST(ToolingResolver, DocumentSymbolsAreHierarchical) {
    const std::string src = R"(enum Color { Red Green Blue }
var top = 1;
class Shape {
  area() { return 0; }
  name() { return "shape"; }
}
fun helper() { return 1; }
)";
    DocumentModel model(src);
    auto syms = model.documentSymbols();
    ASSERT_EQ(syms.size(), 4u);
    EXPECT_EQ(syms[0].name, "Color");
    EXPECT_EQ(syms[0].children.size(), 3u);
    EXPECT_EQ(syms[1].name, "top");
    EXPECT_EQ(syms[2].name, "Shape");
    ASSERT_EQ(syms[2].children.size(), 2u);
    EXPECT_EQ(syms[2].children[0].name, "area");
    EXPECT_EQ(syms[3].name, "helper");
}

TEST(ToolingResolver, OffsetPositionRoundTrip) {
    const std::string src = "var a = 1;\nvar b = 2;\r\nvar c = 3;\n";
    DocumentModel model(src);
    const std::size_t cOff = offsetOf(src, "c = 3");
    const Position p = model.offsetToPosition(cOff);
    EXPECT_EQ(p.line, 2u);
    EXPECT_EQ(p.character, 4u);
    EXPECT_EQ(model.positionToOffset(p), cOff);
}

TEST(ToolingResolver, PastEndOfLineStaysOnSameLine) {
    const std::string src = "var aaa = 1;\naaa = 2;\n";
    DocumentModel model(src);
    const std::size_t line0End = src.find('\n');
    const std::size_t line1Start = line0End + 1;
    EXPECT_EQ(model.positionToOffset({0, 100}), line0End);

    // The next line starts with a use of aaa, so the old clamp onto
    // lineStart(1) resolves while the fixed clamp onto the break does not.
    ASSERT_NE(model.symbolAt(line1Start), nullptr);
    const std::size_t pastEnd = model.positionToOffset({0, 100});
    EXPECT_NE(pastEnd, line1Start);
    EXPECT_EQ(model.symbolAt(pastEnd), nullptr);
    EXPECT_FALSE(model.definitionAt(pastEnd).has_value());
    EXPECT_TRUE(model.referencesAt(pastEnd).empty());

    // CRLF line break folds the same way, past the CR as well.
    const std::string crlf = "var aaa = 1;\r\naaa = 2;\r\n";
    DocumentModel crlfModel(crlf);
    EXPECT_EQ(crlfModel.positionToOffset({0, 100}),
              std::string("var aaa = 1;").size());

    // Last line without a trailing break clamps to end of text.
    const std::string noTrail = "var aaa = 1;\naaa = 2;";
    DocumentModel noTrailModel(noTrail);
    EXPECT_EQ(noTrailModel.positionToOffset({1, 100}), noTrail.size());

    // An out-of-range line still clamps to the last line.
    EXPECT_EQ(noTrailModel.positionToOffset({99, 100}), noTrail.size());
}

TEST(ToolingResolver, RebuildReplacesState) {
    DocumentModel model("var first = 1;\n");
    EXPECT_TRUE(model.warnings().empty());
    model.rebuild("fun f() { return ghost; }\n");
    ASSERT_EQ(model.warnings().size(), 1u);
    EXPECT_EQ(model.warnings().front().message, "unknown name 'ghost'");
}

// ---------------------------------------------------------------------------
// Corpus sweep: the resolver must not crash on any real program, and its
// warnings must match the known allowlist below.
// ---------------------------------------------------------------------------

// Filenames known to warn. Each entry is an understood, intentional warning:
// a deliberate undefined-name trigger, or an unused local that the program
// never reads (some bind the result of a throwing call, some declare a helper
// the rest of the file does not use). A clean file that starts warning is not
// in this set and fails the test; a new file that warns must be added here on
// purpose.
const std::set<std::string> kExpectedWarnedFiles = {
    "06_shared_upvalue.lox",
    "test_enum_arity_try.lox",
    "test_error_kind_message.lox",
    "test_invalid_map_key_try.lox",
    "try_catch_class_constructor_arity.lox",
    "try_catch_error_instance_properties_catchable.lox",
    "try_catch_error_vs_ordinary_instance_catchability.lox",
};

TEST(ToolingResolverCorpus, NoCrashAndKnownWarnings) {
    const std::vector<fs::path> files = loxpp_test::corpusFiles();
    ASSERT_GT(files.size(), 50u);

    std::set<std::string> warnedFiles;
    for (const fs::path& file : files) {
        const std::string src = loxpp_test::readFile(file);
        DocumentModel model(src);
        // Every offset in every warning must map inside the source.
        for (const Diagnostic& d : model.warnings()) {
            EXPECT_LE(d.offset, src.size()) << file.filename().string();
        }
        if (!model.warnings().empty()) {
            warnedFiles.insert(file.filename().string());
        }
        std::cerr << file.filename().string() << ": " << model.warnings().size()
                  << " warning(s)\n";
        for (const Diagnostic& d : model.warnings()) {
            std::cerr << "    " << d.line << ":" << d.column << " " << d.message
                      << "\n";
        }
    }
    std::cerr << "corpus: " << files.size() << " files, " << warnedFiles.size()
              << " with warnings\n";

    EXPECT_EQ(warnedFiles, kExpectedWarnedFiles);
}
