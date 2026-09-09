#pragma once

// Editor-tooling AST for Lox++.
//
// This tree exists only for navigation features (document symbols, go-to-
// definition, find-references). The single-pass bytecode compiler keeps no
// AST, so src/tooling builds its own with a standalone recursive-descent
// parser that reuses only the Scanner.
//
// Memory model: children are std::unique_ptr, owned by their parent; the
// Program owns the whole tree. Downcast a base pointer with its `kind` tag
// (or dynamic_cast in tests).
//
// Spans: every node carries `offset` and `length` -- a byte range into the
// source string passed to parse(). `source.substr(offset, length)` is the
// node's exact source text. All string_view members (names, literal text)
// also borrow that same source buffer, so it must outlive the Program.
//
// Identifier roles: the parser assigns a role only where the grammar fixes
// it. Every `Name` member -- declaration names, parameters, enum constructor
// fields, destructuring targets, the for-in loop variable -- is a definition
// by construction. Every IdentifierExpr is a reference (its `role` field says
// so explicitly). `ClassDecl::superclass` is the one `Name` that is a
// reference, not a definition. Pattern head identifiers (BindingPat, CtorPat,
// ClassPat) are genuinely ambiguous between a fresh binding and a
// constructor reference; the parser records name + span only and leaves the
// role to the resolver.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "token.h"

namespace loxpp::tooling {

enum class IdentRole : std::uint8_t { Definition, Reference };

// A bare name occurrence that is not a full node: it still carries its own
// span so navigation can point at it.
struct Name {
    std::string_view text;
    std::size_t offset = 0;
    std::size_t length = 0;
};

enum class LiteralKind : std::uint8_t { Number, String, True, False, Nil };

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class ExprKind : std::uint8_t {
    Literal,
    Identifier,
    Unary,
    Binary,
    Logical,
    Call,
    Get,
    Index,
    Slice,
    Assign,
    ListLiteral,
    MapLiteral,
    Grouping,
    This,
    Super,
    Match,
};

struct Expr {
    explicit Expr(ExprKind k) : kind(k) {}
    Expr(const Expr&) = delete;
    Expr& operator=(const Expr&) = delete;
    virtual ~Expr() = default;

    ExprKind kind;
    std::size_t offset = 0;
    std::size_t length = 0;
};
using ExprPtr = std::unique_ptr<Expr>;

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Pattern;
using PatternPtr = std::unique_ptr<Pattern>;

struct LiteralExpr : Expr {
    LiteralExpr() : Expr(ExprKind::Literal) {}
    LiteralKind literal_kind = LiteralKind::Nil;
    std::string_view lexeme; // raw source text of the token
};

struct IdentifierExpr : Expr {
    IdentifierExpr() : Expr(ExprKind::Identifier) {}
    std::string_view name;
    IdentRole role = IdentRole::Reference;
};

struct UnaryExpr : Expr {
    UnaryExpr() : Expr(ExprKind::Unary) {}
    TokenType op = TokenType::MINUS;
    ExprPtr operand;
};

struct BinaryExpr : Expr {
    BinaryExpr() : Expr(ExprKind::Binary) {}
    TokenType op = TokenType::PLUS;
    ExprPtr left;
    ExprPtr right;
};

struct LogicalExpr : Expr {
    LogicalExpr() : Expr(ExprKind::Logical) {}
    TokenType op = TokenType::AND;
    ExprPtr left;
    ExprPtr right;
};

struct CallExpr : Expr {
    CallExpr() : Expr(ExprKind::Call) {}
    ExprPtr callee;
    std::vector<ExprPtr> arguments;
};

struct GetExpr : Expr {
    GetExpr() : Expr(ExprKind::Get) {}
    ExprPtr object;
    std::string_view name;
    std::size_t name_offset = 0;
    std::size_t name_length = 0;
};

struct IndexExpr : Expr {
    IndexExpr() : Expr(ExprKind::Index) {}
    ExprPtr object;
    ExprPtr index;
};

struct SliceExpr : Expr {
    SliceExpr() : Expr(ExprKind::Slice) {}
    ExprPtr object;
    ExprPtr start;
    ExprPtr end;
};

// target is an IdentifierExpr, GetExpr, IndexExpr, or -- for `a[x:y] = v`,
// which the real compiler rejects -- a SliceExpr. The tooling parser records
// the shape and leaves rejection to the compiler and the resolver.
struct AssignExpr : Expr {
    AssignExpr() : Expr(ExprKind::Assign) {}
    ExprPtr target;
    ExprPtr value;
};

struct ListLiteralExpr : Expr {
    ListLiteralExpr() : Expr(ExprKind::ListLiteral) {}
    std::vector<ExprPtr> elements;
};

struct MapEntry {
    ExprPtr key;
    ExprPtr value;
};

struct MapLiteralExpr : Expr {
    MapLiteralExpr() : Expr(ExprKind::MapLiteral) {}
    std::vector<MapEntry> entries;
};

struct GroupingExpr : Expr {
    GroupingExpr() : Expr(ExprKind::Grouping) {}
    ExprPtr inner;
};

struct ThisExpr : Expr {
    ThisExpr() : Expr(ExprKind::This) {}
};

struct SuperExpr : Expr {
    SuperExpr() : Expr(ExprKind::Super) {}
    std::string_view name; // the member after `super .`
    std::size_t name_offset = 0;
    std::size_t name_length = 0;
};

// ---------------------------------------------------------------------------
// Match patterns
// ---------------------------------------------------------------------------

enum class PatternKind : std::uint8_t {
    Literal,
    Wildcard,  // `_`
    Binding,   // bare identifier: binding, wildcard, or nullary constructor
    Ctor,      // Name(a, b) -- positional
    Class,     // Name{a, b} -- named fields
    Seq,       // [a, b, ...rest]
    AtBinding, // name @ subPattern
    Or,        // alt or alt or alt
};

struct Pattern {
    explicit Pattern(PatternKind k) : kind(k) {}
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;
    virtual ~Pattern() = default;

    PatternKind kind;
    std::size_t offset = 0;
    std::size_t length = 0;
};

struct LiteralPat : Pattern {
    LiteralPat() : Pattern(PatternKind::Literal) {}
    LiteralKind literal_kind = LiteralKind::Nil;
    std::string_view lexeme;
};

struct WildcardPat : Pattern {
    WildcardPat() : Pattern(PatternKind::Wildcard) {}
};

struct BindingPat : Pattern {
    BindingPat() : Pattern(PatternKind::Binding) {}
    std::string_view name;
    std::size_t name_offset = 0;
    std::size_t name_length = 0;
};

struct CtorPat : Pattern {
    CtorPat() : Pattern(PatternKind::Ctor) {}
    std::string_view name;
    std::size_t name_offset = 0;
    std::size_t name_length = 0;
    std::vector<Name> fields; // each an identifier binding
};

struct ClassPat : Pattern {
    ClassPat() : Pattern(PatternKind::Class) {}
    std::string_view name;
    std::size_t name_offset = 0;
    std::size_t name_length = 0;
    std::vector<Name> fields;
};

struct SeqPatElem {
    std::string_view name;
    std::size_t offset = 0;
    std::size_t length = 0;
    bool is_rest = false; // `...name`
};

struct SeqPat : Pattern {
    SeqPat() : Pattern(PatternKind::Seq) {}
    std::vector<SeqPatElem> elements;
};

struct AtBindingPat : Pattern {
    AtBindingPat() : Pattern(PatternKind::AtBinding) {}
    std::string_view name;
    std::size_t name_offset = 0;
    std::size_t name_length = 0;
    PatternPtr sub;
};

struct OrPat : Pattern {
    OrPat() : Pattern(PatternKind::Or) {}
    std::vector<PatternPtr> alternatives;
};

// One `case` arm. `patterns` holds either a literal comma-chain (each entry a
// LiteralPat) or a single pattern (which is an OrPat when the source joined
// alternatives with `or`). The body is `{ declaration* expression }` -- then
// `body_decls` holds the leading declarations/statements and `body_expr` the
// final expression -- or a bare expression, in which case `body_decls` is
// empty and `body_expr` is the whole body.
struct MatchArm {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::vector<PatternPtr> patterns;
    ExprPtr guard; // nullable
    std::vector<StmtPtr> body_decls;
    ExprPtr body_expr; // nullable only on malformed input
};

struct MatchExpr : Expr {
    MatchExpr() : Expr(ExprKind::Match) {}
    ExprPtr subject;
    std::vector<MatchArm> arms;
};

// ---------------------------------------------------------------------------
// Declarations and statements
// ---------------------------------------------------------------------------

enum class StmtKind : std::uint8_t {
    VarDecl,
    DestructureDecl,
    FunDecl,
    ClassDecl,
    EnumDecl,
    Block,
    If,
    While,
    For,
    ForIn,
    Print,
    Return,
    Break,
    Continue,
    ExprStmt,
};

struct Stmt {
    explicit Stmt(StmtKind k) : kind(k) {}
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    virtual ~Stmt() = default;

    StmtKind kind;
    std::size_t offset = 0;
    std::size_t length = 0;
};

struct Param {
    std::string_view name;
    std::size_t offset = 0;
    std::size_t length = 0;
};

struct VarDecl : Stmt {
    VarDecl() : Stmt(StmtKind::VarDecl) {}
    Name name;
    ExprPtr initializer; // nullable
};

// `var {a, b} = e;` (record form, is_sequence == false) or
// `var [a, b] = e;` (sequence form, is_sequence == true). Targets are
// definitions; `_` in the sequence form is a discard, kept as a target named
// "_" for the resolver to skip.
struct DestructureDecl : Stmt {
    DestructureDecl() : Stmt(StmtKind::DestructureDecl) {}
    bool is_sequence = false;
    std::vector<Name> targets;
    ExprPtr initializer;
};

struct FunDecl : Stmt {
    FunDecl() : Stmt(StmtKind::FunDecl) {}
    Name name;
    std::vector<Param> params;
    std::vector<StmtPtr> body;
};

struct MethodDecl {
    std::size_t offset = 0;
    std::size_t length = 0;
    Name name;
    std::vector<Param> params;
    std::vector<StmtPtr> body;
};

struct ClassDecl : Stmt {
    ClassDecl() : Stmt(StmtKind::ClassDecl) {}
    Name name;
    std::optional<Name> superclass; // a reference, not a definition
    std::vector<MethodDecl> methods;
};

struct EnumCtorDecl {
    std::size_t offset = 0;
    std::size_t length = 0;
    Name name;
    std::vector<Param> fields;
};

struct EnumDecl : Stmt {
    EnumDecl() : Stmt(StmtKind::EnumDecl) {}
    Name name;
    std::vector<EnumCtorDecl> ctors;
};

struct Block : Stmt {
    Block() : Stmt(StmtKind::Block) {}
    std::vector<StmtPtr> body;
};

struct IfStmt : Stmt {
    IfStmt() : Stmt(StmtKind::If) {}
    ExprPtr condition;
    StmtPtr then_branch;
    StmtPtr else_branch; // nullable
};

struct WhileStmt : Stmt {
    WhileStmt() : Stmt(StmtKind::While) {}
    ExprPtr condition;
    StmtPtr body;
};

struct ForStmt : Stmt {
    ForStmt() : Stmt(StmtKind::For) {}
    StmtPtr initializer; // VarDecl, ExprStmt, or null
    ExprPtr condition;   // nullable
    ExprPtr increment;   // nullable
    StmtPtr body;
};

struct ForInStmt : Stmt {
    ForInStmt() : Stmt(StmtKind::ForIn) {}
    Name variable; // scoped to the loop
    ExprPtr iterable;
    StmtPtr body;
};

struct PrintStmt : Stmt {
    PrintStmt() : Stmt(StmtKind::Print) {}
    ExprPtr value;
};

struct ReturnStmt : Stmt {
    ReturnStmt() : Stmt(StmtKind::Return) {}
    ExprPtr value; // nullable
};

struct BreakStmt : Stmt {
    BreakStmt() : Stmt(StmtKind::Break) {}
};

struct ContinueStmt : Stmt {
    ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct ExprStmt : Stmt {
    ExprStmt() : Stmt(StmtKind::ExprStmt) {}
    ExprPtr expr;
};

struct Program {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::vector<StmtPtr> body;
};

} // namespace loxpp::tooling
