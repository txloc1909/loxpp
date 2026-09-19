#include "lsp/code_action.h"

#include <algorithm>
#include <utility>

#include "token.h"
#include "tooling/ast.h"
#include "tooling/document_model.h"

namespace loxpp::lsp {

namespace {

using tooling::BinaryExpr;
using tooling::CallExpr;
using tooling::DocumentModel;
using tooling::Expr;
using tooling::ExprKind;
using tooling::IdentifierExpr;
using tooling::MatchExpr;
using tooling::Stmt;
using tooling::StmtKind;

bool covers(std::size_t begin, std::size_t length, std::size_t at) {
    return begin <= at && at <= begin + length;
}

// -- match search -----------------------------------------------------------

const MatchExpr* matchInExpr(const Expr* e, std::size_t at,
                             const MatchExpr* best);
const MatchExpr* matchInStmt(const Stmt* s, std::size_t at,
                             const MatchExpr* best);

const MatchExpr* matchInStmts(const std::vector<tooling::StmtPtr>& stmts,
                              std::size_t at, const MatchExpr* best) {
    for (const auto& s : stmts) {
        best = matchInStmt(s.get(), at, best);
    }
    return best;
}

const MatchExpr* matchInExpr(const Expr* e, std::size_t at,
                             const MatchExpr* best) {
    if (e == nullptr || !covers(e->offset, e->length, at)) {
        return best;
    }
    if (e->kind == ExprKind::Match) {
        best = static_cast<const MatchExpr*>(e);
    }
    switch (e->kind) {
    case ExprKind::Literal:
    case ExprKind::Identifier:
    case ExprKind::This:
    case ExprKind::Super:
        break;
    case ExprKind::Unary: {
        const auto* u = static_cast<const tooling::UnaryExpr*>(e);
        best = matchInExpr(u->operand.get(), at, best);
        break;
    }
    case ExprKind::Binary: {
        const auto* b = static_cast<const BinaryExpr*>(e);
        best = matchInExpr(b->left.get(), at, best);
        best = matchInExpr(b->right.get(), at, best);
        break;
    }
    case ExprKind::Logical: {
        const auto* l = static_cast<const tooling::LogicalExpr*>(e);
        best = matchInExpr(l->left.get(), at, best);
        best = matchInExpr(l->right.get(), at, best);
        break;
    }
    case ExprKind::Call: {
        const auto* c = static_cast<const CallExpr*>(e);
        best = matchInExpr(c->callee.get(), at, best);
        for (const auto& a : c->arguments) {
            best = matchInExpr(a.get(), at, best);
        }
        break;
    }
    case ExprKind::Get: {
        const auto* g = static_cast<const tooling::GetExpr*>(e);
        best = matchInExpr(g->object.get(), at, best);
        break;
    }
    case ExprKind::Index: {
        const auto* ix = static_cast<const tooling::IndexExpr*>(e);
        best = matchInExpr(ix->object.get(), at, best);
        best = matchInExpr(ix->index.get(), at, best);
        break;
    }
    case ExprKind::Slice: {
        const auto* sl = static_cast<const tooling::SliceExpr*>(e);
        best = matchInExpr(sl->object.get(), at, best);
        best = matchInExpr(sl->start.get(), at, best);
        best = matchInExpr(sl->end.get(), at, best);
        break;
    }
    case ExprKind::Assign: {
        const auto* a = static_cast<const tooling::AssignExpr*>(e);
        best = matchInExpr(a->target.get(), at, best);
        best = matchInExpr(a->value.get(), at, best);
        break;
    }
    case ExprKind::ListLiteral: {
        const auto* l = static_cast<const tooling::ListLiteralExpr*>(e);
        for (const auto& el : l->elements) {
            best = matchInExpr(el.get(), at, best);
        }
        break;
    }
    case ExprKind::MapLiteral: {
        const auto* m = static_cast<const tooling::MapLiteralExpr*>(e);
        for (const auto& entry : m->entries) {
            best = matchInExpr(entry.key.get(), at, best);
            best = matchInExpr(entry.value.get(), at, best);
        }
        break;
    }
    case ExprKind::Grouping: {
        const auto* g = static_cast<const tooling::GroupingExpr*>(e);
        best = matchInExpr(g->inner.get(), at, best);
        break;
    }
    case ExprKind::Match: {
        const auto* m = static_cast<const MatchExpr*>(e);
        best = matchInExpr(m->subject.get(), at, best);
        for (const auto& arm : m->arms) {
            best = matchInExpr(arm.guard.get(), at, best);
            best = matchInStmts(arm.body_decls, at, best);
            best = matchInExpr(arm.body_expr.get(), at, best);
        }
        break;
    }
    }
    return best;
}

const MatchExpr* matchInStmt(const Stmt* s, std::size_t at,
                             const MatchExpr* best) {
    if (s == nullptr || !covers(s->offset, s->length, at)) {
        return best;
    }
    switch (s->kind) {
    case StmtKind::VarDecl: {
        const auto* v = static_cast<const tooling::VarDecl*>(s);
        best = matchInExpr(v->initializer.get(), at, best);
        break;
    }
    case StmtKind::DestructureDecl: {
        const auto* d = static_cast<const tooling::DestructureDecl*>(s);
        best = matchInExpr(d->initializer.get(), at, best);
        break;
    }
    case StmtKind::FunDecl: {
        const auto* f = static_cast<const tooling::FunDecl*>(s);
        best = matchInStmts(f->body, at, best);
        break;
    }
    case StmtKind::ClassDecl: {
        const auto* c = static_cast<const tooling::ClassDecl*>(s);
        for (const auto& m : c->methods) {
            best = matchInStmts(m.body, at, best);
        }
        break;
    }
    case StmtKind::EnumDecl:
    case StmtKind::Break:
    case StmtKind::Continue:
        break;
    case StmtKind::Block: {
        const auto* b = static_cast<const tooling::Block*>(s);
        best = matchInStmts(b->body, at, best);
        break;
    }
    case StmtKind::If: {
        const auto* i = static_cast<const tooling::IfStmt*>(s);
        best = matchInExpr(i->condition.get(), at, best);
        best = matchInStmt(i->then_branch.get(), at, best);
        best = matchInStmt(i->else_branch.get(), at, best);
        break;
    }
    case StmtKind::While: {
        const auto* w = static_cast<const tooling::WhileStmt*>(s);
        best = matchInExpr(w->condition.get(), at, best);
        best = matchInStmt(w->body.get(), at, best);
        break;
    }
    case StmtKind::For: {
        const auto* f = static_cast<const tooling::ForStmt*>(s);
        best = matchInStmt(f->initializer.get(), at, best);
        best = matchInExpr(f->condition.get(), at, best);
        best = matchInExpr(f->increment.get(), at, best);
        best = matchInStmt(f->body.get(), at, best);
        break;
    }
    case StmtKind::ForIn: {
        const auto* f = static_cast<const tooling::ForInStmt*>(s);
        best = matchInExpr(f->iterable.get(), at, best);
        best = matchInStmt(f->body.get(), at, best);
        break;
    }
    case StmtKind::Try: {
        const auto* t = static_cast<const tooling::TryStmt*>(s);
        best = matchInStmt(t->try_block.get(), at, best);
        best = matchInStmt(t->catch_block.get(), at, best);
        break;
    }
    case StmtKind::Throw: {
        const auto* t = static_cast<const tooling::ThrowStmt*>(s);
        best = matchInExpr(t->value.get(), at, best);
        break;
    }
    case StmtKind::Defer: {
        const auto* d = static_cast<const tooling::DeferStmt*>(s);
        best = matchInExpr(d->call.get(), at, best);
        break;
    }
    case StmtKind::Print: {
        const auto* p = static_cast<const tooling::PrintStmt*>(s);
        best = matchInExpr(p->value.get(), at, best);
        break;
    }
    case StmtKind::Return: {
        const auto* r = static_cast<const tooling::ReturnStmt*>(s);
        best = matchInExpr(r->value.get(), at, best);
        break;
    }
    case StmtKind::ExprStmt: {
        const auto* e = static_cast<const tooling::ExprStmt*>(s);
        best = matchInExpr(e->expr.get(), at, best);
        break;
    }
    }
    return best;
}

// -- plus search ------------------------------------------------------------

const BinaryExpr* plusInExpr(const Expr* e, std::size_t at,
                             const BinaryExpr* best);
const BinaryExpr* plusInStmt(const Stmt* s, std::size_t at,
                             const BinaryExpr* best);

const BinaryExpr* plusInStmts(const std::vector<tooling::StmtPtr>& stmts,
                              std::size_t at, const BinaryExpr* best) {
    for (const auto& s : stmts) {
        best = plusInStmt(s.get(), at, best);
    }
    return best;
}

const BinaryExpr* plusInExpr(const Expr* e, std::size_t at,
                             const BinaryExpr* best) {
    if (e == nullptr || !covers(e->offset, e->length, at)) {
        return best;
    }
    if (e->kind == ExprKind::Binary) {
        const auto* b = static_cast<const BinaryExpr*>(e);
        if (b->op == TokenType::PLUS) {
            best = b;
        }
        best = plusInExpr(b->left.get(), at, best);
        best = plusInExpr(b->right.get(), at, best);
        return best;
    }
    // Same shape as matchInExpr; only Match arms and Binary children matter,
    // but every child must be visited so a `+` nested anywhere is found.
    switch (e->kind) {
    case ExprKind::Literal:
    case ExprKind::Identifier:
    case ExprKind::This:
    case ExprKind::Super:
    case ExprKind::Binary:
        break;
    case ExprKind::Unary: {
        const auto* u = static_cast<const tooling::UnaryExpr*>(e);
        best = plusInExpr(u->operand.get(), at, best);
        break;
    }
    case ExprKind::Logical: {
        const auto* l = static_cast<const tooling::LogicalExpr*>(e);
        best = plusInExpr(l->left.get(), at, best);
        best = plusInExpr(l->right.get(), at, best);
        break;
    }
    case ExprKind::Call: {
        const auto* c = static_cast<const CallExpr*>(e);
        best = plusInExpr(c->callee.get(), at, best);
        for (const auto& a : c->arguments) {
            best = plusInExpr(a.get(), at, best);
        }
        break;
    }
    case ExprKind::Get: {
        const auto* g = static_cast<const tooling::GetExpr*>(e);
        best = plusInExpr(g->object.get(), at, best);
        break;
    }
    case ExprKind::Index: {
        const auto* ix = static_cast<const tooling::IndexExpr*>(e);
        best = plusInExpr(ix->object.get(), at, best);
        best = plusInExpr(ix->index.get(), at, best);
        break;
    }
    case ExprKind::Slice: {
        const auto* sl = static_cast<const tooling::SliceExpr*>(e);
        best = plusInExpr(sl->object.get(), at, best);
        best = plusInExpr(sl->start.get(), at, best);
        best = plusInExpr(sl->end.get(), at, best);
        break;
    }
    case ExprKind::Assign: {
        const auto* a = static_cast<const tooling::AssignExpr*>(e);
        best = plusInExpr(a->target.get(), at, best);
        best = plusInExpr(a->value.get(), at, best);
        break;
    }
    case ExprKind::ListLiteral: {
        const auto* l = static_cast<const tooling::ListLiteralExpr*>(e);
        for (const auto& el : l->elements) {
            best = plusInExpr(el.get(), at, best);
        }
        break;
    }
    case ExprKind::MapLiteral: {
        const auto* m = static_cast<const tooling::MapLiteralExpr*>(e);
        for (const auto& entry : m->entries) {
            best = plusInExpr(entry.key.get(), at, best);
            best = plusInExpr(entry.value.get(), at, best);
        }
        break;
    }
    case ExprKind::Grouping: {
        const auto* g = static_cast<const tooling::GroupingExpr*>(e);
        best = plusInExpr(g->inner.get(), at, best);
        break;
    }
    case ExprKind::Match: {
        const auto* m = static_cast<const MatchExpr*>(e);
        best = plusInExpr(m->subject.get(), at, best);
        for (const auto& arm : m->arms) {
            best = plusInExpr(arm.guard.get(), at, best);
            best = plusInStmts(arm.body_decls, at, best);
            best = plusInExpr(arm.body_expr.get(), at, best);
        }
        break;
    }
    }
    return best;
}

const BinaryExpr* plusInStmt(const Stmt* s, std::size_t at,
                             const BinaryExpr* best) {
    if (s == nullptr || !covers(s->offset, s->length, at)) {
        return best;
    }
    switch (s->kind) {
    case StmtKind::VarDecl: {
        const auto* v = static_cast<const tooling::VarDecl*>(s);
        best = plusInExpr(v->initializer.get(), at, best);
        break;
    }
    case StmtKind::DestructureDecl: {
        const auto* d = static_cast<const tooling::DestructureDecl*>(s);
        best = plusInExpr(d->initializer.get(), at, best);
        break;
    }
    case StmtKind::FunDecl: {
        const auto* f = static_cast<const tooling::FunDecl*>(s);
        best = plusInStmts(f->body, at, best);
        break;
    }
    case StmtKind::ClassDecl: {
        const auto* c = static_cast<const tooling::ClassDecl*>(s);
        for (const auto& m : c->methods) {
            best = plusInStmts(m.body, at, best);
        }
        break;
    }
    case StmtKind::EnumDecl:
    case StmtKind::Break:
    case StmtKind::Continue:
        break;
    case StmtKind::Block: {
        const auto* b = static_cast<const tooling::Block*>(s);
        best = plusInStmts(b->body, at, best);
        break;
    }
    case StmtKind::If: {
        const auto* i = static_cast<const tooling::IfStmt*>(s);
        best = plusInExpr(i->condition.get(), at, best);
        best = plusInStmt(i->then_branch.get(), at, best);
        best = plusInStmt(i->else_branch.get(), at, best);
        break;
    }
    case StmtKind::While: {
        const auto* w = static_cast<const tooling::WhileStmt*>(s);
        best = plusInExpr(w->condition.get(), at, best);
        best = plusInStmt(w->body.get(), at, best);
        break;
    }
    case StmtKind::For: {
        const auto* f = static_cast<const tooling::ForStmt*>(s);
        best = plusInStmt(f->initializer.get(), at, best);
        best = plusInExpr(f->condition.get(), at, best);
        best = plusInExpr(f->increment.get(), at, best);
        best = plusInStmt(f->body.get(), at, best);
        break;
    }
    case StmtKind::ForIn: {
        const auto* f = static_cast<const tooling::ForInStmt*>(s);
        best = plusInExpr(f->iterable.get(), at, best);
        best = plusInStmt(f->body.get(), at, best);
        break;
    }
    case StmtKind::Try: {
        const auto* t = static_cast<const tooling::TryStmt*>(s);
        best = plusInStmt(t->try_block.get(), at, best);
        best = plusInStmt(t->catch_block.get(), at, best);
        break;
    }
    case StmtKind::Throw: {
        const auto* t = static_cast<const tooling::ThrowStmt*>(s);
        best = plusInExpr(t->value.get(), at, best);
        break;
    }
    case StmtKind::Defer: {
        const auto* d = static_cast<const tooling::DeferStmt*>(s);
        best = plusInExpr(d->call.get(), at, best);
        break;
    }
    case StmtKind::Print: {
        const auto* p = static_cast<const tooling::PrintStmt*>(s);
        best = plusInExpr(p->value.get(), at, best);
        break;
    }
    case StmtKind::Return: {
        const auto* r = static_cast<const tooling::ReturnStmt*>(s);
        best = plusInExpr(r->value.get(), at, best);
        break;
    }
    case StmtKind::ExprStmt: {
        const auto* e = static_cast<const tooling::ExprStmt*>(s);
        best = plusInExpr(e->expr.get(), at, best);
        break;
    }
    }
    return best;
}

// -- small helpers ----------------------------------------------------------

bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

bool isIdentifier(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    const char first = s.front();
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          first == '_')) {
        return false;
    }
    for (char c : s) {
        if (!isWordChar(c)) {
            return false;
        }
    }
    return true;
}

// Leading whitespace of the line that holds `offset`.
std::string lineIndent(const std::string& text, std::size_t offset) {
    offset = std::min(offset, text.size());
    std::size_t start = (offset == 0) ? 0 : text.rfind('\n', offset - 1);
    start = (start == std::string::npos) ? 0 : start + 1;
    std::size_t i = start;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
        ++i;
    }
    return text.substr(start, i - start);
}

bool isStrCall(const Expr* e) {
    if (e == nullptr || e->kind != ExprKind::Call) {
        return false;
    }
    const auto* c = static_cast<const CallExpr*>(e);
    if (c->arguments.size() != 1 || c->callee == nullptr ||
        c->callee->kind != ExprKind::Identifier) {
        return false;
    }
    return static_cast<const IdentifierExpr*>(c->callee.get())->name == "str";
}

} // namespace

std::optional<MissingMatchArms>
parseNonExhaustiveMatch(const std::string& message) {
    constexpr std::string_view kPrefix = "Non-exhaustive match on enum '";
    // kInfix starts after the closing quote (restPos is past it).
    constexpr std::string_view kInfix = ": missing arms for: ";
    if (message.compare(0, kPrefix.size(), kPrefix) != 0) {
        return std::nullopt;
    }
    const std::size_t enumEnd = message.find('\'', kPrefix.size());
    if (enumEnd == std::string::npos) {
        return std::nullopt;
    }
    std::string enumName =
        message.substr(kPrefix.size(), enumEnd - kPrefix.size());
    const std::size_t restPos = enumEnd + 1;
    if (message.compare(restPos, kInfix.size(), kInfix) != 0) {
        return std::nullopt;
    }
    const std::string rest = message.substr(restPos + kInfix.size());
    if (rest.empty() || !isIdentifier(enumName)) {
        return std::nullopt;
    }
    std::vector<std::string> missing;
    std::size_t p = 0;
    while (true) {
        const std::size_t comma = rest.find(',', p);
        std::string part = (comma == std::string::npos)
                               ? rest.substr(p)
                               : rest.substr(p, comma - p);
        const std::size_t a = part.find_first_not_of(' ');
        const std::size_t b = part.find_last_not_of(' ');
        part = (a == std::string::npos) ? "" : part.substr(a, b - a + 1);
        if (!isIdentifier(part)) {
            return std::nullopt;
        }
        missing.push_back(std::move(part));
        if (comma == std::string::npos) {
            break;
        }
        p = comma + 1;
    }
    return MissingMatchArms{std::move(enumName), std::move(missing)};
}

std::vector<QuickFix> matchExhaustivenessFixes(const DocumentModel& model,
                                               std::size_t requestOffset,
                                               const json& contextDiagnostics) {
    std::vector<QuickFix> out;
    if (!contextDiagnostics.is_array()) {
        return out;
    }
    const std::string& text = model.text();
    // (title, insert offset) pairs already emitted. One diagnostic per arm
    // set is normal, but a repeated diagnostic must not offer the same arms
    // twice: applying both would duplicate each arm.
    std::vector<std::pair<std::string, std::size_t>> seen;
    for (std::size_t i = 0; i < contextDiagnostics.size(); ++i) {
        const json& d = contextDiagnostics[i];
        if (!d.is_object()) {
            continue;
        }
        std::string message;
        tooling::Position diagPos{0, 0};
        try {
            message = d.at("message").get<std::string>();
            diagPos.line =
                d.at("range").at("start").at("line").get<std::size_t>();
            diagPos.character =
                d.at("range").at("start").at("character").get<std::size_t>();
        } catch (const std::exception&) {
            continue;
        }
        const std::optional<MissingMatchArms> parsed =
            parseNonExhaustiveMatch(message);
        if (!parsed.has_value()) {
            continue;
        }
        const std::size_t at = model.positionToOffset(diagPos);
        const MatchExpr* match = nullptr;
        for (const auto& s : model.program().body) {
            match = matchInStmt(s.get(), at, match);
        }
        if (match == nullptr && at != requestOffset) {
            for (const auto& s : model.program().body) {
                match = matchInStmt(s.get(), requestOffset, match);
            }
        }
        if (match == nullptr) {
            continue;
        }
        // The insert point is the match's closing brace: the last non-blank
        // character of the match span must be `}`.
        const std::size_t end =
            std::min(match->offset + match->length, text.size());
        std::size_t insert = end;
        while (insert > match->offset &&
               (text[insert - 1] == ' ' || text[insert - 1] == '\t' ||
                text[insert - 1] == '\n' || text[insert - 1] == '\r')) {
            --insert;
        }
        if (insert == match->offset || text[insert - 1] != '}') {
            continue;
        }
        --insert;
        std::string indent = "  ";
        if (!match->arms.empty()) {
            indent = lineIndent(text, match->arms.front().offset);
        }
        // Indent before the brace would sit alone on its line after a pure
        // insertion. Fold that run into the edit and re-emit it after the
        // new arms, so no line holds only whitespace and the brace keeps
        // its indent.
        std::size_t editAt = insert;
        std::size_t editLen = 0;
        std::string braceIndent;
        {
            std::size_t w = insert;
            while (w > match->offset &&
                   (text[w - 1] == ' ' || text[w - 1] == '\t')) {
                --w;
            }
            if (w != insert && w > match->offset && text[w - 1] == '\n') {
                editAt = w;
                editLen = insert - w;
                braceIndent = text.substr(w, insert - w);
            }
        }
        std::string edit;
        if (editLen == 0 && insert > 0 && text[insert - 1] != '\n') {
            edit += "\n";
        }
        for (const std::string& name : parsed->missing) {
            edit += indent + "case " + name + " => nil\n";
        }
        edit += braceIndent;
        std::string title = parsed->missing.size() == 1
                                ? "Add missing match arm: " + parsed->missing[0]
                                : "Add missing match arms: ";
        if (parsed->missing.size() > 1) {
            for (std::size_t k = 0; k < parsed->missing.size(); ++k) {
                title += parsed->missing[k];
                if (k + 1 < parsed->missing.size()) {
                    title += ", ";
                }
            }
        }
        const auto key = std::make_pair(title, editAt);
        if (std::ranges::find(seen, key) != seen.end()) {
            continue;
        }
        seen.push_back(key);
        QuickFix fix;
        fix.title = std::move(title);
        fix.edits.push_back(SourceEdit{editAt, editLen, std::move(edit)});
        fix.diagnosticIndex = i;
        out.push_back(std::move(fix));
    }
    return out;
}

std::vector<QuickFix> strWrapFixes(const DocumentModel& model,
                                   std::size_t requestOffset) {
    const BinaryExpr* plus = nullptr;
    for (const auto& s : model.program().body) {
        plus = plusInStmt(s.get(), requestOffset, plus);
    }
    if (plus == nullptr || plus->left == nullptr || plus->right == nullptr) {
        return {};
    }
    std::vector<QuickFix> out;
    if (!isStrCall(plus->left.get())) {
        QuickFix fix;
        fix.title = "Wrap left operand in str()";
        fix.edits.push_back(SourceEdit{plus->left->offset, 0, "str("});
        fix.edits.push_back(
            SourceEdit{plus->left->offset + plus->left->length, 0, ")"});
        out.push_back(std::move(fix));
    }
    if (!isStrCall(plus->right.get())) {
        QuickFix fix;
        fix.title = "Wrap right operand in str()";
        fix.edits.push_back(SourceEdit{plus->right->offset, 0, "str("});
        fix.edits.push_back(
            SourceEdit{plus->right->offset + plus->right->length, 0, ")"});
        out.push_back(std::move(fix));
    }
    return out;
}

} // namespace loxpp::lsp
