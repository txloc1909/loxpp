#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
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
    const std::string src = "fun f() { enum Local { A B } return 0; }\n";
    DocumentModel model(src);
    ASSERT_FALSE(warningMessages(model).empty());
    EXPECT_EQ(model.warnings().front().message,
              "enum must be declared at global scope");
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

TEST(ToolingResolver, RebuildReplacesState) {
    DocumentModel model("var first = 1;\n");
    EXPECT_TRUE(model.warnings().empty());
    model.rebuild("fun f() { return ghost; }\n");
    ASSERT_EQ(model.warnings().size(), 1u);
    EXPECT_EQ(model.warnings().front().message, "unknown name 'ghost'");
}

// ---------------------------------------------------------------------------
// Corpus sweep: the resolver must not crash on any real program, and the
// undefined-name warning must stay quiet on the curated examples.
// ---------------------------------------------------------------------------

TEST(ToolingResolverCorpus, NoCrashAndFewWarnings) {
    const fs::path root = LOXPP_PROJECT_SOURCE_DIR;
    std::vector<fs::path> files;
    for (const char* dir :
         {"examples", "bootstrap", "test/translation-probes"}) {
        const fs::path base = root / dir;
        if (!fs::is_directory(base)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(base)) {
            if (entry.is_regular_file() && entry.path().extension() == ".lox") {
                files.push_back(entry.path());
            }
        }
    }
    ASSERT_GT(files.size(), 50u);
    std::sort(files.begin(), files.end());

    std::size_t totalWarnings = 0;
    for (const fs::path& file : files) {
        const std::string src = readFile(file);
        DocumentModel model(src);
        // Every offset in every warning must map inside the source.
        for (const Diagnostic& d : model.warnings()) {
            EXPECT_LE(d.offset, src.size()) << file.filename().string();
        }
        const std::size_t n = model.warnings().size();
        totalWarnings += n;
        std::cerr << file.filename().string() << ": " << n << " warning(s)\n";
        for (const Diagnostic& d : model.warnings()) {
            std::cerr << "    " << d.line << ":" << d.column << " " << d.message
                      << "\n";
        }
    }
    std::cerr << "corpus: " << files.size() << " files, " << totalWarnings
              << " warnings\n";
    // The curated corpus is clean Lox++; a handful of lint hits is the ceiling.
    EXPECT_LE(totalWarnings, 5u);
}
