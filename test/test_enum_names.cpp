// A variable of any binding form must not have the name of an enum, also
// before the enum declaration, and a member name may have it
// (spec/02-syntax.md, enum declaration). None of these programs reads the
// enum name, so no other check can reject them.

#include "analyze.h"

#include <gtest/gtest.h>

#include <string>

namespace {

constexpr const char* kMessage =
    "A variable can't have the name of the enum 'E'.";

struct Case {
    const char* name;
    const char* source;
    std::size_t line;
};

const Case kCases[] = {
    {"GlobalVar", "enum E { A }\nvar E = 1;\n", 2},
    {"GlobalVarBeforeEnum", "var E = 1;\nenum E { A }\n", 1},
    {"GlobalFun", "enum E { A }\nfun E() {}\n", 2},
    {"GlobalClass", "enum E { A }\nclass E {}\n", 2},
    {"GlobalMapDestructure", "enum E { A }\nvar {E} = nil;\n", 2},
    {"GlobalSeqDestructure", "enum E { A }\nvar [E] = [1];\n", 2},
    {"BlockVar", "enum E { A }\n{ var E = 1; }\n", 2},
    {"LocalVar", "enum E { A }\nfun f() { var E = 1; }\n", 2},
    {"Parameter", "fun f(E) {}\nenum E { A }\n", 1},
    {"MethodParameter", "enum E { A }\nclass C { m(E) {} }\n", 2},
    {"LocalFun", "enum E { A }\nfun f() { fun E() {} }\n", 2},
    {"LocalClass", "enum E { A }\nfun f() { class E {} }\n", 2},
    {"LocalMapDestructure", "enum E { A }\nfun f() { var {E} = nil; }\n", 2},
    {"LocalSeqDestructure", "enum E { A }\nfun f() { var [E] = [1]; }\n", 2},
    {"CatchVariable", "enum E { A }\ntry {} catch (E) {}\n", 2},
    {"ForInVariable", "enum E { A }\nfor (var E in []) {}\n", 2},
    {"ForVariable", "enum E { A }\nfor (var E = 0; false;) {}\n", 2},
    {"PatternBinding", "enum E { A }\nvar x = match 1 { case E => 1 };\n", 2},
    {"AtBinding",
     "enum E { A }\nvar x = match [1] { case E @ [y] => y case _ => 0 };\n", 2},
    {"SequenceElement",
     "enum E { A }\nvar x = match [1] { case [E] => 1 case _ => 0 };\n", 2},
    {"SequenceRest",
     "enum E { A }\nvar x = match [1] { case [...E] => 1 case _ => 0 };\n", 2},
    {"PositionalField",
     "enum E { A }\nenum F { B(v) }\nvar x = match F.B(1) { case F.B(E) => 1 "
     "};\n",
     3},
    {"NamedField",
     "enum E { A }\nenum F { B(E) }\nvar x = match F.B(1) { case F.B{E} => 1 "
     "};\n",
     3},
    {"ClassPatternField",
     "enum E { A }\nclass C {}\nvar x = match C() { case C{E} => 1 case _ => 0 "
     "};\n",
     3},
};

class EnumNameAsVariable : public ::testing::TestWithParam<Case> {};

TEST_P(EnumNameAsVariable, IsACompileError) {
    const Case& c = GetParam();
    const std::vector<Diagnostic> diags = analyze(c.source);
    // Error recovery can add a second error after the first one.
    ASSERT_FALSE(diags.empty()) << "source:\n" << c.source;
    EXPECT_EQ(diags[0].message, kMessage) << "source:\n" << c.source;
    EXPECT_EQ(diags[0].line, c.line) << "source:\n" << c.source;
}

INSTANTIATE_TEST_SUITE_P(EveryBindingForm, EnumNameAsVariable,
                         ::testing::ValuesIn(kCases),
                         [](const ::testing::TestParamInfo<Case>& info) {
                             return std::string(info.param.name);
                         });

TEST(EnumNameAsMember, IsAllowed) {
    const char* source = "enum E { E(E) F }\n"
                         "class C {\n"
                         "    E() { return this.E; }\n"
                         "}\n"
                         "fun g(c) { c.E = 1; return c.E; }\n"
                         "var v = E.E(1);\n";
    const std::vector<Diagnostic> diags = analyze(source);
    EXPECT_TRUE(diags.empty()) << "first: " << diags.front().message;
}

} // namespace
