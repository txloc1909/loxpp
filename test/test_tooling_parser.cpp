#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tooling/ast.h"
#include "tooling/tooling_parser.h"

using namespace loxpp::tooling;

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::vector<fs::path> corpusFiles() {
    const fs::path root = LOXPP_PROJECT_SOURCE_DIR;
    std::vector<fs::path> out;
    for (const char* dir :
         {"examples", "bootstrap", "test/translation-probes"}) {
        const fs::path base = root / dir;
        if (!fs::is_directory(base)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(base)) {
            if (entry.is_regular_file() && entry.path().extension() == ".lox") {
                out.push_back(entry.path());
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// A full span audit: every node of every kind -- Stmt, Expr, Pattern -- plus
// every bare Name, Param, and sub-name span (`.name`, `super.name`, pattern
// heads, sequence elements, arm ranges) must lie inside [0, source.size()].
// N7 walks these same spans, so an out-of-bounds one is a latent crash there.
struct SpanAudit {
    std::string_view src;
    std::string file;
    std::size_t nodes = 0;

    void inBounds(std::size_t offset, std::size_t length, const char* what) {
        ++nodes;
        EXPECT_LE(offset, src.size()) << what << " offset in " << file;
        EXPECT_LE(offset + length, src.size()) << what << " end in " << file;
    }

    void name(const Name& n) {
        inBounds(n.offset, n.length, "Name");
        if (!n.text.empty()) {
            EXPECT_GE(n.text.data(), src.data()) << "Name text under " << file;
            EXPECT_LE(n.text.data() + n.text.size(), src.data() + src.size())
                << "Name text over " << file;
        }
    }
    void param(const Param& p) { inBounds(p.offset, p.length, "Param"); }

    void pattern(const Pattern* p) {
        if (p == nullptr) {
            return;
        }
        inBounds(p->offset, p->length, "Pattern");
        switch (p->kind) {
        case PatternKind::Binding: {
            const auto* b = static_cast<const BindingPat*>(p);
            inBounds(b->name_offset, b->name_length, "BindingPat.name");
            break;
        }
        case PatternKind::Ctor: {
            const auto* c = static_cast<const CtorPat*>(p);
            inBounds(c->name_offset, c->name_length, "CtorPat.name");
            for (const auto& f : c->fields) {
                name(f);
            }
            break;
        }
        case PatternKind::Class: {
            const auto* c = static_cast<const ClassPat*>(p);
            inBounds(c->name_offset, c->name_length, "ClassPat.name");
            for (const auto& f : c->fields) {
                name(f);
            }
            break;
        }
        case PatternKind::Seq: {
            const auto* s = static_cast<const SeqPat*>(p);
            for (const auto& e : s->elements) {
                inBounds(e.offset, e.length, "SeqPatElem");
            }
            break;
        }
        case PatternKind::AtBinding: {
            const auto* a = static_cast<const AtBindingPat*>(p);
            inBounds(a->name_offset, a->name_length, "AtBindingPat.name");
            pattern(a->sub.get());
            break;
        }
        case PatternKind::Or:
            for (const auto& alt : static_cast<const OrPat*>(p)->alternatives) {
                pattern(alt.get());
            }
            break;
        default:
            break;
        }
    }

    void arm(const MatchArm& a) {
        inBounds(a.offset, a.length, "MatchArm");
        for (const auto& pat : a.patterns) {
            pattern(pat.get());
        }
        expr(a.guard.get());
        for (const auto& d : a.body_decls) {
            stmt(d.get());
        }
        expr(a.body_expr.get());
    }

    void expr(const Expr* e) {
        if (e == nullptr) {
            return;
        }
        inBounds(e->offset, e->length, "Expr");
        switch (e->kind) {
        case ExprKind::Unary:
            expr(static_cast<const UnaryExpr*>(e)->operand.get());
            break;
        case ExprKind::Binary: {
            const auto* b = static_cast<const BinaryExpr*>(e);
            expr(b->left.get());
            expr(b->right.get());
            break;
        }
        case ExprKind::Logical: {
            const auto* b = static_cast<const LogicalExpr*>(e);
            expr(b->left.get());
            expr(b->right.get());
            break;
        }
        case ExprKind::Call: {
            const auto* c = static_cast<const CallExpr*>(e);
            expr(c->callee.get());
            for (const auto& a : c->arguments) {
                expr(a.get());
            }
            break;
        }
        case ExprKind::Get: {
            const auto* g = static_cast<const GetExpr*>(e);
            expr(g->object.get());
            inBounds(g->name_offset, g->name_length, "GetExpr.name");
            break;
        }
        case ExprKind::Index: {
            const auto* i = static_cast<const IndexExpr*>(e);
            expr(i->object.get());
            expr(i->index.get());
            break;
        }
        case ExprKind::Slice: {
            const auto* sl = static_cast<const SliceExpr*>(e);
            expr(sl->object.get());
            expr(sl->start.get());
            expr(sl->end.get());
            break;
        }
        case ExprKind::Assign: {
            const auto* a = static_cast<const AssignExpr*>(e);
            expr(a->target.get());
            expr(a->value.get());
            break;
        }
        case ExprKind::ListLiteral:
            for (const auto& el :
                 static_cast<const ListLiteralExpr*>(e)->elements) {
                expr(el.get());
            }
            break;
        case ExprKind::MapLiteral:
            for (const auto& en :
                 static_cast<const MapLiteralExpr*>(e)->entries) {
                expr(en.key.get());
                expr(en.value.get());
            }
            break;
        case ExprKind::Grouping:
            expr(static_cast<const GroupingExpr*>(e)->inner.get());
            break;
        case ExprKind::Super: {
            const auto* s = static_cast<const SuperExpr*>(e);
            inBounds(s->name_offset, s->name_length, "SuperExpr.name");
            break;
        }
        case ExprKind::Match: {
            const auto* m = static_cast<const MatchExpr*>(e);
            expr(m->subject.get());
            for (const auto& a : m->arms) {
                arm(a);
            }
            break;
        }
        default:
            break;
        }
    }

    void body(const std::vector<StmtPtr>& stmts) {
        for (const auto& s : stmts) {
            stmt(s.get());
        }
    }

    void stmt(const Stmt* s) {
        if (s == nullptr) {
            return;
        }
        inBounds(s->offset, s->length, "Stmt");
        switch (s->kind) {
        case StmtKind::VarDecl: {
            const auto* v = static_cast<const VarDecl*>(s);
            name(v->name);
            expr(v->initializer.get());
            break;
        }
        case StmtKind::DestructureDecl: {
            const auto* d = static_cast<const DestructureDecl*>(s);
            for (const auto& t : d->targets) {
                name(t);
            }
            expr(d->initializer.get());
            break;
        }
        case StmtKind::FunDecl: {
            const auto* f = static_cast<const FunDecl*>(s);
            name(f->name);
            for (const auto& p : f->params) {
                param(p);
            }
            body(f->body);
            break;
        }
        case StmtKind::ClassDecl: {
            const auto* c = static_cast<const ClassDecl*>(s);
            name(c->name);
            if (c->superclass.has_value()) {
                name(*c->superclass);
            }
            for (const auto& m : c->methods) {
                inBounds(m.offset, m.length, "MethodDecl");
                name(m.name);
                for (const auto& p : m.params) {
                    param(p);
                }
                body(m.body);
            }
            break;
        }
        case StmtKind::EnumDecl: {
            const auto* en = static_cast<const EnumDecl*>(s);
            name(en->name);
            for (const auto& ctor : en->ctors) {
                inBounds(ctor.offset, ctor.length, "EnumCtorDecl");
                name(ctor.name);
                for (const auto& fld : ctor.fields) {
                    param(fld);
                }
            }
            break;
        }
        case StmtKind::Block:
            body(static_cast<const Block*>(s)->body);
            break;
        case StmtKind::If: {
            const auto* i = static_cast<const IfStmt*>(s);
            expr(i->condition.get());
            stmt(i->then_branch.get());
            stmt(i->else_branch.get());
            break;
        }
        case StmtKind::While: {
            const auto* w = static_cast<const WhileStmt*>(s);
            expr(w->condition.get());
            stmt(w->body.get());
            break;
        }
        case StmtKind::For: {
            const auto* f = static_cast<const ForStmt*>(s);
            stmt(f->initializer.get());
            expr(f->condition.get());
            expr(f->increment.get());
            stmt(f->body.get());
            break;
        }
        case StmtKind::ForIn: {
            const auto* f = static_cast<const ForInStmt*>(s);
            name(f->variable);
            expr(f->iterable.get());
            stmt(f->body.get());
            break;
        }
        case StmtKind::Print:
            expr(static_cast<const PrintStmt*>(s)->value.get());
            break;
        case StmtKind::Return:
            expr(static_cast<const ReturnStmt*>(s)->value.get());
            break;
        case StmtKind::ExprStmt:
            expr(static_cast<const ExprStmt*>(s)->expr.get());
            break;
        default:
            break;
        }
    }
};

// Audit every span in a parsed Program against its source buffer.
std::size_t auditSpans(const Program& prog, std::string_view src,
                       const std::string& file) {
    SpanAudit audit{src, file, 0};
    EXPECT_LE(prog.offset, src.size()) << "Program offset in " << file;
    EXPECT_LE(prog.offset + prog.length, src.size())
        << "Program end in " << file;
    audit.body(prog.body);
    return audit.nodes;
}

} // namespace

TEST(ToolingParserCorpus, ParsesEveryFileWithoutCrash) {
    const std::vector<fs::path> files = corpusFiles();
    // examples/*.lox (62) + bootstrap/*.lox (2) + translation-probes/*.lox (47)
    ASSERT_EQ(files.size(), 111U);

    std::size_t totalNodes = 0;
    for (const auto& file : files) {
        const std::string src = readFile(file);
        const Program prog = parse(src);
        EXPECT_FALSE(prog.body.empty())
            << "empty Program for " << file.string();

        // Every span of every node kind -- Stmt, Expr, Pattern, and every
        // bare Name / sub-name -- stays inside the source buffer.
        totalNodes += auditSpans(prog, src, file.string());
    }
    EXPECT_GT(totalNodes, 0U);
}

TEST(ToolingParserSpans, IdentifierAndCallSpansAreExact) {
    const std::string src = "fun greet(name) {\n"
                            "    var msg = \"hi\";\n"
                            "    print msg;\n"
                            "}\n"
                            "\n"
                            "var picked = match greet {\n"
                            "    case Leaf(v) => v\n"
                            "    case _ => 0\n"
                            "};\n";

    const Program prog = parse(src);
    ASSERT_EQ(prog.body.size(), 2U);

    // 1. FunDecl name span.
    const auto* fn = dynamic_cast<const FunDecl*>(prog.body[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name.offset, src.find("greet"));
    EXPECT_EQ(fn->name.length, 5U);
    EXPECT_EQ(src.substr(fn->name.offset, fn->name.length), "greet");

    // 2. A specific IdentifierExpr span: `msg` inside `print msg;`.
    ASSERT_EQ(fn->body.size(), 2U);
    const auto* printStmt = dynamic_cast<const PrintStmt*>(fn->body[1].get());
    ASSERT_NE(printStmt, nullptr);
    const auto* msgRef =
        dynamic_cast<const IdentifierExpr*>(printStmt->value.get());
    ASSERT_NE(msgRef, nullptr);
    const std::size_t printMsgAt = src.find("print msg") + 6;
    EXPECT_EQ(msgRef->offset, printMsgAt);
    EXPECT_EQ(msgRef->length, 3U);
    EXPECT_EQ(src.substr(msgRef->offset, msgRef->length), "msg");
    EXPECT_EQ(msgRef->role, IdentRole::Reference);

    // 3. MatchArm span: `case Leaf(v) => v`.
    const auto* varDecl = dynamic_cast<const VarDecl*>(prog.body[1].get());
    ASSERT_NE(varDecl, nullptr);
    const auto* matchExpr =
        dynamic_cast<const MatchExpr*>(varDecl->initializer.get());
    ASSERT_NE(matchExpr, nullptr);
    ASSERT_EQ(matchExpr->arms.size(), 2U);
    const MatchArm& arm0 = matchExpr->arms[0];
    const std::size_t armStart = src.find("case Leaf");
    const std::size_t armEnd = src.find("=> v") + 4;
    EXPECT_EQ(arm0.offset, armStart);
    EXPECT_EQ(arm0.length, armEnd - armStart);
    EXPECT_EQ(src.substr(arm0.offset, arm0.length), "case Leaf(v) => v");

    // The constructor pattern records its name and field spans.
    ASSERT_EQ(arm0.patterns.size(), 1U);
    const auto* ctor = dynamic_cast<const CtorPat*>(arm0.patterns[0].get());
    ASSERT_NE(ctor, nullptr);
    EXPECT_EQ(src.substr(ctor->name_offset, ctor->name_length), "Leaf");
    ASSERT_EQ(ctor->fields.size(), 1U);
    EXPECT_EQ(src.substr(ctor->fields[0].offset, ctor->fields[0].length), "v");

    // The second arm is a wildcard.
    EXPECT_EQ(matchExpr->arms[1].patterns.size(), 1U);
    EXPECT_EQ(matchExpr->arms[1].patterns[0]->kind, PatternKind::Wildcard);

    std::cout << "span check (offset,length -> text):\n"
              << "  FunDecl name    : " << fn->name.offset << ","
              << fn->name.length << " -> \""
              << src.substr(fn->name.offset, fn->name.length) << "\"\n"
              << "  IdentifierExpr  : " << msgRef->offset << ","
              << msgRef->length << " -> \""
              << src.substr(msgRef->offset, msgRef->length) << "\"\n"
              << "  MatchArm[0]     : " << arm0.offset << "," << arm0.length
              << " -> \"" << src.substr(arm0.offset, arm0.length) << "\"\n";
}

TEST(ToolingParserSpans, GetExprAndSuperNameSpans) {
    const std::string src = "class B < A {\n"
                            "    run() { return super.step() + this.count; }\n"
                            "}\n";
    const Program prog = parse(src);
    ASSERT_EQ(prog.body.size(), 1U);
    const auto* cls = dynamic_cast<const ClassDecl*>(prog.body[0].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_TRUE(cls->superclass.has_value());
    EXPECT_EQ(src.substr(cls->superclass->offset, cls->superclass->length),
              "A");
    ASSERT_EQ(cls->methods.size(), 1U);

    const auto* ret =
        dynamic_cast<const ReturnStmt*>(cls->methods[0].body[0].get());
    ASSERT_NE(ret, nullptr);
    const auto* sum = dynamic_cast<const BinaryExpr*>(ret->value.get());
    ASSERT_NE(sum, nullptr);

    const auto* superCall = dynamic_cast<const CallExpr*>(sum->left.get());
    ASSERT_NE(superCall, nullptr);
    const auto* superGet =
        dynamic_cast<const SuperExpr*>(superCall->callee.get());
    ASSERT_NE(superGet, nullptr);
    EXPECT_EQ(src.substr(superGet->name_offset, superGet->name_length), "step");

    const auto* thisGet = dynamic_cast<const GetExpr*>(sum->right.get());
    ASSERT_NE(thisGet, nullptr);
    EXPECT_EQ(src.substr(thisGet->name_offset, thisGet->name_length), "count");
    EXPECT_EQ(thisGet->object->kind, ExprKind::This);
}

TEST(ToolingParserRecovery, GarbageBetweenDeclarationsStillYieldsLater) {
    // Removing the synchronize() call in declaration() makes this fail:
    // the parser never reaches `b`.
    const std::string src = "fun a(){} @#$ fun b(){}";
    const Program prog = parse(src);
    bool sawA = false;
    bool sawB = false;
    for (const auto& s : prog.body) {
        if (const auto* f = dynamic_cast<const FunDecl*>(s.get())) {
            sawA = sawA || f->name.text == "a";
            sawB = sawB || f->name.text == "b";
        }
    }
    EXPECT_TRUE(sawA);
    EXPECT_TRUE(sawB);
}

TEST(ToolingParserRecovery, MissingInitializerStillYieldsLater) {
    const std::string src = "var x = ; var y = 1;";
    const Program prog = parse(src);
    bool sawY = false;
    for (const auto& s : prog.body) {
        if (const auto* v = dynamic_cast<const VarDecl*>(s.get())) {
            sawY = sawY || v->name.text == "y";
        }
    }
    EXPECT_TRUE(sawY);
}

TEST(ToolingParserRecovery, BadStatementInsideBlockDoesNotLoseTheRest) {
    const std::string src = "fun f() { var a = 1; @@@ var b = 2; }";
    const Program prog = parse(src);
    ASSERT_EQ(prog.body.size(), 1U);
    const auto* fn = dynamic_cast<const FunDecl*>(prog.body[0].get());
    ASSERT_NE(fn, nullptr);
    bool sawB = false;
    for (const auto& s : fn->body) {
        if (const auto* v = dynamic_cast<const VarDecl*>(s.get())) {
            sawB = sawB || v->name.text == "b";
        }
    }
    EXPECT_TRUE(sawB);
}

TEST(ToolingParserDestructure, TargetsAreRecordedWithSpans) {
    const std::string src = "var [head, _, tail] = xs;\n";
    const Program prog = parse(src);
    ASSERT_EQ(prog.body.size(), 1U);
    const auto* d = dynamic_cast<const DestructureDecl*>(prog.body[0].get());
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->is_sequence);
    ASSERT_EQ(d->targets.size(), 3U);
    EXPECT_EQ(d->targets[0].text, "head");
    EXPECT_EQ(d->targets[1].text, "_");
    EXPECT_EQ(d->targets[2].text, "tail");
    EXPECT_EQ(src.substr(d->targets[2].offset, d->targets[2].length), "tail");
}

TEST(ToolingParserMatch, BlockFormArmBodyEndsInExpression) {
    const std::string src = "var r = match e {\n"
                            "    case X => { helper(e); result }\n"
                            "};\n";
    const Program prog = parse(src);
    const auto* v = dynamic_cast<const VarDecl*>(prog.body[0].get());
    ASSERT_NE(v, nullptr);
    const auto* m = dynamic_cast<const MatchExpr*>(v->initializer.get());
    ASSERT_NE(m, nullptr);
    ASSERT_EQ(m->arms.size(), 1U);
    EXPECT_EQ(m->arms[0].body_decls.size(), 1U);
    ASSERT_NE(m->arms[0].body_expr, nullptr);
    const auto* tail =
        dynamic_cast<const IdentifierExpr*>(m->arms[0].body_expr.get());
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(tail->name, "result");
}

TEST(ToolingParserMatch, SliceOnAssignLeftIsRecordedNotRejected) {
    const std::string src = "xs[a:b] = ys;";
    const Program prog = parse(src);
    ASSERT_EQ(prog.body.size(), 1U);
    const auto* es = dynamic_cast<const ExprStmt*>(prog.body[0].get());
    ASSERT_NE(es, nullptr);
    const auto* assign = dynamic_cast<const AssignExpr*>(es->expr.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->target->kind, ExprKind::Slice);
}

TEST(ToolingParserEnum, MisplacedEnumStillParses) {
    const std::string src = "fun f() { enum E { A B } }";
    const Program prog = parse(src);
    ASSERT_EQ(prog.body.size(), 1U);
    const auto* fn = dynamic_cast<const FunDecl*>(prog.body[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_EQ(fn->body.size(), 1U);
    const auto* en = dynamic_cast<const EnumDecl*>(fn->body[0].get());
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(en->name.text, "E");
    EXPECT_EQ(en->ctors.size(), 2U);
}

// R1: a first token that cannot start a declaration or statement, and a
// lone unterminated string, must not underflow the token cursor. Each one
// returns a Program (possibly empty) with every span inside the buffer.
TEST(ToolingParserAdversarial, FirstTokenCannotStartAnythingDoesNotCrash) {
    const std::vector<std::string> inputs = {".",   ")",    "]",
                                             "}",   ",",    "*",
                                             "/",   "%",    ":",
                                             "?",   "@",    "=>",
                                             "==",  "!=",   "<=",
                                             ">=",  "<",    ">",
                                             "...", "=",    "and",
                                             "or",  "case", "else",
                                             "in",  "if)",  "\"unterminated",
                                             "\"",  "@#$",  ""};

    for (const auto& src : inputs) {
        const Program prog = parse(src);
        // No assertion on body size: an empty Program is a valid result here.
        const std::size_t nodes = auditSpans(prog, src, "input<" + src + ">");
        EXPECT_LE(prog.offset + prog.length, src.size())
            << "input<" << src << ">";
        (void)nodes;
    }
}

// R1 companion: the same unexpected leading token followed by real code --
// the parser must recover and still surface the later declaration.
TEST(ToolingParserAdversarial, RecoversAfterUnexpectedLeadingToken) {
    const std::string src = ") fun after() { var x = 1; }";
    const Program prog = parse(src);
    bool sawAfter = false;
    for (const auto& s : prog.body) {
        if (const auto* f = dynamic_cast<const FunDecl*>(s.get())) {
            sawAfter = sawAfter || f->name.text == "after";
        }
    }
    EXPECT_TRUE(sawAfter);
}

// R2: deeply nested constructs must not overflow the C++ call stack. Each
// case returns a Program with in-bounds spans in bounded time.
TEST(ToolingParserAdversarial, DeepNestingDoesNotOverflowStack) {
    constexpr int kDepth = 10000;

    struct Case {
        const char* name;
        std::string open;
        std::string mid;
        std::string close;
    };
    const std::vector<Case> cases = {
        {"parens", "var x = " + std::string(kDepth, '(') + "1", std::string(),
         std::string(kDepth, ')') + ";"},
        {"brackets", "var x = " + std::string(kDepth, '['), "1",
         std::string(kDepth, ']') + ";"},
        {"braces", std::string(kDepth, '{'), std::string(),
         std::string(kDepth, '}')},
        {"unary", "var x = " + std::string(kDepth, '!'), "y", ";"},
    };

    for (const auto& c : cases) {
        std::string src;
        src.reserve(c.open.size() + c.mid.size() + c.close.size());
        src += c.open;
        src += c.mid;
        src += c.close;

        const auto begin = std::chrono::steady_clock::now();
        const Program prog = parse(src);
        const auto elapsed = std::chrono::steady_clock::now() - begin;

        auditSpans(prog, src, std::string("deep-") + c.name);
        EXPECT_LT(
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
            10)
            << c.name << " parse did not finish in bounded time";
    }

    // Nested call chain: f(f(f(...(0)...))).
    {
        std::string src = "var x = ";
        for (int i = 0; i < kDepth; ++i) {
            src += "f(";
        }
        src += "0";
        src += std::string(kDepth, ')');
        src += ";";
        const Program prog = parse(src);
        auditSpans(prog, src, "deep-calls");
    }

    // Nested match subjects: match match match ... x {} {} {}.
    {
        std::string src = "var x = ";
        for (int i = 0; i < kDepth; ++i) {
            src += "match ";
        }
        src += "x ";
        src += std::string(kDepth, '{');
        src += std::string(kDepth, '}');
        src += ";";
        const Program prog = parse(src);
        auditSpans(prog, src, "deep-match");
    }
}
