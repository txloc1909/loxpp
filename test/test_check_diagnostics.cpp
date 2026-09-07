#include "analyze.h"

#include <gtest/gtest.h>

#include <string>

namespace {

// One diagnostic, with the exact line and column and a message substring.
void expectOne(const std::string& source, std::size_t line, std::size_t column,
               const std::string& messagePart) {
    std::vector<Diagnostic> diags = analyze(source);
    ASSERT_EQ(diags.size(), 1U) << "source:\n" << source;
    EXPECT_EQ(diags[0].line, line);
    EXPECT_EQ(diags[0].column, column);
    EXPECT_EQ(diags[0].severity, Severity::Error);
    EXPECT_NE(diags[0].message.find(messagePart), std::string::npos)
        << "message was: " << diags[0].message;
}

TEST(CheckDiagnostics, BreakAtTopLevel) {
    expectOne("break;", 1, 1, "'break' outside a loop");
}

TEST(CheckDiagnostics, ReturnAtTopLevel) {
    expectOne("return 1;", 1, 1, "return from top-level code");
}

TEST(CheckDiagnostics, ReadLocalInOwnInitializer) {
    // The reference to 'a' on the right-hand side is the offending token.
    expectOne("{\n  var a = a;\n}\n", 2, 11,
              "read local variable in its own initializer");
}

TEST(CheckDiagnostics, DuplicateLocalInBlock) {
    expectOne("{\n  var x;\n  var x;\n}\n", 3, 7,
              "Already a variable with this name in this scope");
}

TEST(CheckDiagnostics, UnterminatedString) {
    // Column comes from the opening quote offset, not from the token line,
    // which the scanner has already advanced past the newline.
    expectOne("var s = \"abc;\n", 1, 9, "Unterminated string");
}

TEST(CheckDiagnostics, NonExhaustiveMatchOnEnum) {
    expectOne("enum Coin { Heads Tails }\n"
              "var r = match Heads() { case Heads => 1 };\n",
              2, 41, "Non-exhaustive match on enum 'Coin'");
}

TEST(CheckDiagnostics, MissingExpressionAtEnd) {
    // 'print 1 +' with nothing after the '+': the error token is EOF.
    expectOne("print 1 +;", 1, 11, "Expect expression");
}

TEST(CheckDiagnostics, CleanSourceHasNoDiagnostics) {
    EXPECT_TRUE(analyze("var x = 1;\nprint x + 2;\n").empty());
}

TEST(CheckDiagnostics, ExhaustiveEnumMatchHasNoPhantom) {
    // A dense, JUMP_TABLE-eligible match drives previewEnumArms, which forces
    // panic mode during its speculative scan. No diagnostic must leak.
    EXPECT_TRUE(analyze("enum Dir { N S E W }\n"
                        "var r = match N() { case N => 1 case S => 2 "
                        "case E => 3 case W => 4 };\n")
                    .empty());
}

TEST(CheckDiagnostics, PanicModeCollapsesToOneDiagnostic) {
    // Two syntax errors in one statement; panic mode keeps it to one.
    std::vector<Diagnostic> diags = analyze("var 1 = = ;");
    EXPECT_EQ(diags.size(), 1U);
}

} // namespace
