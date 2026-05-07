#pragma once

#include "function.h"
#include "parser.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class MemoryManager;
class Compiler;

static constexpr int UINT8_COUNT = 256;

ObjFunction* compile(const std::string& source, MemoryManager* mm);

enum class FunctionType { SCRIPT, FUNCTION, METHOD, INITIALIZER };

struct ClassCompiler {
    ClassCompiler* enclosing{nullptr};
    bool hasSuperclass{false};
};

struct Local {
    Token name;
    int depth; // -1 = declared but not yet initialized; >=0 = scope depth
    bool isCaptured{false};
};

struct Upvalue {
    uint8_t index;
    bool isLocal; // true = captures a local of the immediately enclosing fn
};

struct LoopContext {
    int start;      // LOOP target: condition (while) or increment (for)
    int localCount; // m_localCount at loop entry, for cleanup POPs
    std::vector<int>
        breakJumps; // JUMP placeholder offsets to patch on loop exit
};

struct ConstructorInfo {
    std::string enumName;
    uint8_t tag;
    uint8_t arity;
    std::vector<std::string> fieldNames;
};

class Compiler {
  public:
    Compiler(ObjFunction* function, Parser* parser, MemoryManager* mm,
             FunctionType type = FunctionType::SCRIPT,
             Compiler* enclosing = nullptr);
    ~Compiler();

    Chunk* getCurrentChunk() const { return &m_function->chunk; }
    void endCompiler();
    void markRoots(MemoryManager& mm) const;

    void expression();
    void grouping();
    void unary();
    void binary();
    void literal();
    void number();
    void string();
    void variable();
    void call();

    void declaration();
    void statement();
    void printStatement();
    void expressionStatement();
    void ifStatement();
    void whileStatement();
    void forStatement();
    void forInStatement(const Token& itemName);
    void breakStatement();
    void continueStatement();
    void matchExpression();
    void enumDeclaration();

    struct MatchArmResult {
        int lastMiss;     // patch offset for literal miss (-1 = ident pat)
        int guardMiss;    // patch offset for guard fail  (-1 = no guard)
        int bindingCount; // locals pushed by the pattern
        bool isUnguardedCatchAll;           // true → suppress MATCH_ERROR
        std::vector<std::string> ctorNames; // constructors matched by this arm
    };
    MatchArmResult compileMatchArm(int subjectSlot, int armLocalBase,
                                   int resultSlot);
    void varDeclaration();
    void block();
    void funDeclaration();
    void classDeclaration();
    void method();
    void returnStatement();

    void and_();
    void or_();
    void dot();
    void this_();
    void super_();
    void listLiteral();
    void mapLiteral();
    void subscript();

  private:
    void beginScope();
    void endScope();

    void addLocal(const Token& name);
    int resolveLocal(const Token& name) const;
    int addUpvalue(uint8_t index, bool isLocal);
    int resolveUpvalue(const Token& name);
    void declareVariable();
    void markInitialized();

    void varDestructure();
    void emitDestructureLocal(const std::vector<Token>& fields);
    void emitDestructureGlobal(const std::vector<Token>& fields);

    void compileMatchBody();
    void parseFunction(FunctionType type);

    void emitByte(Byte byte);
    void emitByte(Op op);
    void emitBytes(Op op, Byte byte);
    void emitReturn();
    int emitJump(Op op);
    void patchJump(int offset);
    void emitLoop(int loopStart);
    void emitLoopCleanup(int targetLocalCount);

    uint8_t makeConstant(Value value);
    uint8_t identifierConstant(const Token& name);
    void namedVariable(const Token& name, bool canAssign);

    // Walks the enclosing chain to the root compiler.
    Compiler* findRootCompiler();
    const ConstructorInfo* findConstructor(const std::string& name) const;
    bool findClass(const std::string& name) const;
    // Emit GET_INDEX bindings for a constructor pattern.
    // Returns the number of locals bound.
    int compileCtorPattern(int subjectSlot, const ConstructorInfo& info,
                           const Token& patTok);
    // Emit GET_PROPERTY bindings for a class pattern (named-field only).
    // Returns the number of locals bound.
    int compileClassPattern(int subjectSlot, const Token& patTok);
    // Raise a compile error if seen constructor arms don't cover all variants.
    void checkEnumExhaustiveness(const std::set<std::string>& seenCtors);

    ObjFunction* m_function;
    Parser* m_parser;
    MemoryManager* m_mm;
    FunctionType m_type;
    Compiler* m_enclosing;
    ClassCompiler* m_currentClass{nullptr};

    Local m_locals[UINT8_COUNT];
    int m_localCount{0};
    int m_scopeDepth{0};
    Upvalue m_upvalues[UINT8_COUNT];
    int m_upvalueCount{0};
    std::vector<LoopContext> m_loopStack;

    // Constructor table — only meaningfully populated on the root compiler.
    std::unordered_map<std::string, ConstructorInfo> m_constructors;
    // enumName → ordered list of ctor names, for exhaustiveness checking.
    std::unordered_map<std::string, std::vector<std::string>> m_enumCtors;
    // Class name table — only meaningfully populated on the root compiler.
    std::set<std::string> m_classNames;
};
