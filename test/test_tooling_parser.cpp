#include <algorithm>
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

// Walk every expression in the tree and hand each to `visit`.
template <class F>
void forEachExpr(const Expr* e, const F& visit);
template <class F>
void forEachExpr(const Stmt* s, const F& visit);

template <class F>
void forEachExpr(const MatchArm& arm, const F& visit) {
    forEachExpr(arm.guard.get(), visit);
    for (const auto& d : arm.body_decls) {
        forEachExpr(d.get(), visit);
    }
    forEachExpr(arm.body_expr.get(), visit);
}

template <class F>
void forEachExpr(const Expr* e, const F& visit) {
    if (e == nullptr) {
        return;
    }
    visit(e);
    switch (e->kind) {
    case ExprKind::Unary:
        forEachExpr(static_cast<const UnaryExpr*>(e)->operand.get(), visit);
        break;
    case ExprKind::Binary: {
        const auto* b = static_cast<const BinaryExpr*>(e);
        forEachExpr(b->left.get(), visit);
        forEachExpr(b->right.get(), visit);
        break;
    }
    case ExprKind::Logical: {
        const auto* b = static_cast<const LogicalExpr*>(e);
        forEachExpr(b->left.get(), visit);
        forEachExpr(b->right.get(), visit);
        break;
    }
    case ExprKind::Call: {
        const auto* c = static_cast<const CallExpr*>(e);
        forEachExpr(c->callee.get(), visit);
        for (const auto& a : c->arguments) {
            forEachExpr(a.get(), visit);
        }
        break;
    }
    case ExprKind::Get:
        forEachExpr(static_cast<const GetExpr*>(e)->object.get(), visit);
        break;
    case ExprKind::Index: {
        const auto* i = static_cast<const IndexExpr*>(e);
        forEachExpr(i->object.get(), visit);
        forEachExpr(i->index.get(), visit);
        break;
    }
    case ExprKind::Slice: {
        const auto* sl = static_cast<const SliceExpr*>(e);
        forEachExpr(sl->object.get(), visit);
        forEachExpr(sl->start.get(), visit);
        forEachExpr(sl->end.get(), visit);
        break;
    }
    case ExprKind::Assign: {
        const auto* a = static_cast<const AssignExpr*>(e);
        forEachExpr(a->target.get(), visit);
        forEachExpr(a->value.get(), visit);
        break;
    }
    case ExprKind::ListLiteral:
        for (const auto& el :
             static_cast<const ListLiteralExpr*>(e)->elements) {
            forEachExpr(el.get(), visit);
        }
        break;
    case ExprKind::MapLiteral:
        for (const auto& en : static_cast<const MapLiteralExpr*>(e)->entries) {
            forEachExpr(en.key.get(), visit);
            forEachExpr(en.value.get(), visit);
        }
        break;
    case ExprKind::Grouping:
        forEachExpr(static_cast<const GroupingExpr*>(e)->inner.get(), visit);
        break;
    case ExprKind::Match: {
        const auto* m = static_cast<const MatchExpr*>(e);
        forEachExpr(m->subject.get(), visit);
        for (const auto& arm : m->arms) {
            forEachExpr(arm, visit);
        }
        break;
    }
    default:
        break;
    }
}

template <class F>
void forEachExpr(const std::vector<StmtPtr>& body, const F& visit) {
    for (const auto& s : body) {
        forEachExpr(s.get(), visit);
    }
}

template <class F>
void forEachExpr(const Stmt* s, const F& visit) {
    if (s == nullptr) {
        return;
    }
    switch (s->kind) {
    case StmtKind::VarDecl:
        forEachExpr(static_cast<const VarDecl*>(s)->initializer.get(), visit);
        break;
    case StmtKind::DestructureDecl:
        forEachExpr(static_cast<const DestructureDecl*>(s)->initializer.get(),
                    visit);
        break;
    case StmtKind::FunDecl:
        forEachExpr(static_cast<const FunDecl*>(s)->body, visit);
        break;
    case StmtKind::ClassDecl:
        for (const auto& m : static_cast<const ClassDecl*>(s)->methods) {
            forEachExpr(m.body, visit);
        }
        break;
    case StmtKind::Block:
        forEachExpr(static_cast<const Block*>(s)->body, visit);
        break;
    case StmtKind::If: {
        const auto* i = static_cast<const IfStmt*>(s);
        forEachExpr(i->condition.get(), visit);
        forEachExpr(i->then_branch.get(), visit);
        forEachExpr(i->else_branch.get(), visit);
        break;
    }
    case StmtKind::While: {
        const auto* w = static_cast<const WhileStmt*>(s);
        forEachExpr(w->condition.get(), visit);
        forEachExpr(w->body.get(), visit);
        break;
    }
    case StmtKind::For: {
        const auto* f = static_cast<const ForStmt*>(s);
        forEachExpr(f->initializer.get(), visit);
        forEachExpr(f->condition.get(), visit);
        forEachExpr(f->increment.get(), visit);
        forEachExpr(f->body.get(), visit);
        break;
    }
    case StmtKind::ForIn: {
        const auto* f = static_cast<const ForInStmt*>(s);
        forEachExpr(f->iterable.get(), visit);
        forEachExpr(f->body.get(), visit);
        break;
    }
    case StmtKind::Print:
        forEachExpr(static_cast<const PrintStmt*>(s)->value.get(), visit);
        break;
    case StmtKind::Return:
        forEachExpr(static_cast<const ReturnStmt*>(s)->value.get(), visit);
        break;
    case StmtKind::ExprStmt:
        forEachExpr(static_cast<const ExprStmt*>(s)->expr.get(), visit);
        break;
    default:
        break;
    }
}

} // namespace

TEST(ToolingParserCorpus, ParsesEveryFileWithoutCrash) {
    const std::vector<fs::path> files = corpusFiles();
    // examples/*.lox (62) + bootstrap/*.lox (2) + translation-probes/*.lox (47)
    ASSERT_EQ(files.size(), 111U);

    for (const auto& file : files) {
        const std::string src = readFile(file);
        const Program prog = parse(src);
        EXPECT_FALSE(prog.body.empty())
            << "empty Program for " << file.string();

        // Every span stays inside the source buffer.
        forEachExpr(prog.body, [&](const Expr* e) {
            EXPECT_LE(e->offset, src.size()) << file.string();
            EXPECT_LE(e->offset + e->length, src.size()) << file.string();
        });
    }
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
