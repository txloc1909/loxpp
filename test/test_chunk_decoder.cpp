// test_chunk_decoder.cpp — decoder/chunk-walker round-trip tests.
//
// The checkpoint (notes/backend-implementation-dag.md, node N0): re-disassemble
// every chunk in the compiled ObjFunction tree with the new decoder, and
// compare the result against disassembleChunk (src/debug.cpp) — the oracle —
// string for string. `renderInstruction` below turns a DecodedInstruction
// back into that exact text, independently of debug.cpp's own formatting
// code, so a decoding bug shows up as a text mismatch against the oracle.
//
// Corpora exercised:
//   1. test/translation-probes/*.lox — the probes the DAG analysis is
//      grounded in.
//   2. examples/*.lox — the wider example corpus.
//   3. bootstrap/loxpp_interpreter.lox — the self-hosted interpreter, the
//      largest and most structurally varied program in the repo.

#include "backend/chunk_decoder.h"
#include "compiler.h"
#include "debug.h"
#include "exec_objects.h"
#include "memory_manager.h"
#include "object.h"
#include "value.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef LOXPP_PROJECT_SOURCE_DIR
#error                                                                         \
    "LOXPP_PROJECT_SOURCE_DIR must be defined by the build (see test/CMakeLists.txt)"
#endif

namespace {

namespace fs = std::filesystem;

fs::path projectRoot() { return fs::path(LOXPP_PROJECT_SOURCE_DIR); }

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

std::vector<fs::path> listLoxFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".lox") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Single source of truth for "every Op enumerator", used by both `mnemonic`
// (below) and `allOps` (R1/R3 fix: the opcode-coverage test needs to name
// every enumerator, and this list is also the reason `mnemonic`'s switch has
// no `default:` — a new Op with no entry here is a compile-time -Wswitch
// warning at that switch, not a silent decoder gap).
#define LOXPP_FOR_EACH_OP(X)                                                   \
    X(CONSTANT)                                                                \
    X(NIL)                                                                     \
    X(TRUE)                                                                    \
    X(FALSE)                                                                   \
    X(EQUAL)                                                                   \
    X(GREATER)                                                                 \
    X(LESS)                                                                    \
    X(NEGATE)                                                                  \
    X(ADD)                                                                     \
    X(SUBTRACT)                                                                \
    X(MULTIPLY)                                                                \
    X(DIVIDE)                                                                  \
    X(MODULO)                                                                  \
    X(NOT)                                                                     \
    X(PRINT)                                                                   \
    X(POP)                                                                     \
    X(GET_LOCAL)                                                               \
    X(SET_LOCAL)                                                               \
    X(DEFINE_GLOBAL)                                                           \
    X(GET_GLOBAL)                                                              \
    X(SET_GLOBAL)                                                              \
    X(JUMP)                                                                    \
    X(JUMP_IF_FALSE)                                                           \
    X(LOOP)                                                                    \
    X(CALL)                                                                    \
    X(RETURN)                                                                  \
    X(CLOSURE)                                                                 \
    X(GET_UPVALUE)                                                             \
    X(SET_UPVALUE)                                                             \
    X(CLOSE_UPVALUE)                                                           \
    X(CLASS)                                                                   \
    X(GET_PROPERTY)                                                            \
    X(SET_PROPERTY)                                                            \
    X(DEFINE_METHOD)                                                           \
    X(INVOKE)                                                                  \
    X(INHERIT)                                                                 \
    X(GET_SUPER)                                                               \
    X(SUPER_INVOKE)                                                            \
    X(BUILD_LIST)                                                              \
    X(BUILD_MAP)                                                               \
    X(GET_INDEX)                                                               \
    X(SET_INDEX)                                                               \
    X(SLICE)                                                                   \
    X(IN)                                                                      \
    X(GET_ITER)                                                                \
    X(ITER_HAS_NEXT)                                                           \
    X(ITER_NEXT)                                                               \
    X(MATCH_ERROR)                                                             \
    X(JUMP_TABLE)                                                              \
    X(GET_TAG)                                                                 \
    X(INSTANCEOF)                                                              \
    X(IS_SEQ)

// Re-renders one decoded instruction into the exact text
// disassembleInstruction (src/debug.cpp) prints for it with color disabled.
// The category groups mirror decodeOne() in chunk_decoder.cpp; each mnemonic
// is generated from the Op enumerator's own spelling, since debug.cpp always
// uses that spelling verbatim.
const char* mnemonic(Op op) {
    switch (op) {
#define LOXPP_MNEMONIC_CASE(name)                                              \
    case Op::name:                                                             \
        return #name;
        LOXPP_FOR_EACH_OP(LOXPP_MNEMONIC_CASE)
#undef LOXPP_MNEMONIC_CASE
    }
    return "UNKNOWN";
}

// Every Op enumerator, in declaration order. Used by the opcode-coverage
// test (R1) to fail when the corpus never exercises one of them.
std::vector<Op> allOps() {
    return {
#define LOXPP_OP_VALUE(name) Op::name,
        LOXPP_FOR_EACH_OP(LOXPP_OP_VALUE)
#undef LOXPP_OP_VALUE
    };
}

void renderInstruction(const Chunk& chunk, const DecodedInstruction& ins,
                       std::ostream& out) {
    out << ins.offset << ": ";
    switch (ins.op) {
    case Op::NIL:
    case Op::TRUE:
    case Op::FALSE:
    case Op::EQUAL:
    case Op::GREATER:
    case Op::LESS:
    case Op::NEGATE:
    case Op::ADD:
    case Op::SUBTRACT:
    case Op::MULTIPLY:
    case Op::DIVIDE:
    case Op::MODULO:
    case Op::NOT:
    case Op::PRINT:
    case Op::POP:
    case Op::RETURN:
    case Op::CLOSE_UPVALUE:
    case Op::INHERIT:
    case Op::GET_INDEX:
    case Op::SET_INDEX:
    case Op::SLICE:
    case Op::IN:
    case Op::GET_ITER:
    case Op::ITER_HAS_NEXT:
    case Op::ITER_NEXT:
    case Op::MATCH_ERROR:
    case Op::GET_TAG:
    case Op::IS_SEQ:
        out << mnemonic(ins.op) << '\n';
        return;

    case Op::CONSTANT:
    case Op::DEFINE_GLOBAL:
    case Op::GET_GLOBAL:
    case Op::SET_GLOBAL:
    case Op::CLASS:
    case Op::GET_PROPERTY:
    case Op::SET_PROPERTY:
    case Op::DEFINE_METHOD:
    case Op::GET_SUPER:
    case Op::INSTANCEOF:
        out << mnemonic(ins.op) << ' ' << ins.constantIndex << " ('"
            << stringify(
                   chunk.getConstant(static_cast<uint16_t>(ins.constantIndex)))
            << "')\n";
        return;

    case Op::GET_LOCAL:
    case Op::SET_LOCAL:
    case Op::CALL:
    case Op::BUILD_LIST:
    case Op::BUILD_MAP:
    case Op::GET_UPVALUE:
    case Op::SET_UPVALUE:
        out << mnemonic(ins.op) << ' ' << ins.byteOperand << '\n';
        return;

    case Op::JUMP:
    case Op::JUMP_IF_FALSE:
    case Op::LOOP:
        out << mnemonic(ins.op) << ' ' << ins.offset << " -> " << ins.jumpTarget
            << '\n';
        return;

    case Op::INVOKE:
    case Op::SUPER_INVOKE:
        out << mnemonic(ins.op) << ' ' << ins.constantIndex << " ('"
            << stringify(
                   chunk.getConstant(static_cast<uint16_t>(ins.constantIndex)))
            << "') " << ins.byteOperand << '\n';
        return;

    case Op::CLOSURE:
        out << mnemonic(ins.op) << ' ' << ins.constantIndex << " ('"
            << stringify(
                   chunk.getConstant(static_cast<uint16_t>(ins.constantIndex)))
            << "')\n";
        for (const ClosureUpvalue& up : ins.upvalues) {
            out << "           |  " << (up.isLocal ? "local" : "upvalue") << ' '
                << static_cast<int>(up.index) << '\n';
        }
        return;

    case Op::JUMP_TABLE:
        out << "JUMP_TABLE min=" << ins.minTag
            << " count=" << ins.jumpTable.size() << '\n';
        for (const JumpTableArm& arm : ins.jumpTable) {
            out << "           | tag " << arm.tag << " -> " << arm.target
                << '\n';
        }
        return;
    }
}

std::string renderChunk(const DecodedFunction& node) {
    std::ostringstream out;
    out << "== " << node.displayName << " ==\n";
    for (const DecodedInstruction& ins : node.instructions) {
        renderInstruction(node.function->chunk, ins, out);
    }
    return out.str();
}

std::string oracleDisassembly(const DecodedFunction& node,
                              const MemoryManager& mm) {
    std::ostringstream out;
    disassembleChunk(node.function->chunk, mm, node.displayName.c_str(), out,
                     /*color=*/false);
    return out.str();
}

// R2 fix: the number of function-typed constants in `chunk`'s own pool must
// equal `nested.size()`, at every level of the walk — not only at the root.
// A walker that silently drops functions below some depth (proven possible
// by the reviewer) fails this at the first dropped level, even though every
// node it does produce still renders correct text.
int countFunctionConstants(const Chunk& chunk) {
    int count = 0;
    const ValueArray& constants = chunk.constants();
    for (uint16_t i = 0; i < constants.size(); i++) {
        Value v = constants.at(i);
        if (is<Obj*>(v) && isObjType(as<Obj*>(v), ObjType::FUNCTION)) {
            count++;
        }
    }
    return count;
}

// Recursively checks one function node against the oracle, then its nested
// functions. `path` accumulates display names for a readable failure trace.
void checkNode(const DecodedFunction& node, const MemoryManager& mm,
               const std::string& path) {
    SCOPED_TRACE("function id=" + node.id + " path=" + path);

    EXPECT_EQ(renderChunk(node), oracleDisassembly(node, mm))
        << "decoded text diverges from disassembleChunk";

    const Chunk& chunk = node.function->chunk;
    int cursor =
        node.instructions.empty()
            ? 0
            : node.instructions.back().offset + node.instructions.back().length;
    EXPECT_EQ(cursor, static_cast<int>(chunk.size()))
        << "decoder did not land exactly on the chunk end";

    EXPECT_EQ(static_cast<int>(node.nested.size()),
              countFunctionConstants(chunk))
        << "walker found a different number of nested functions than the "
        << "chunk's own constant pool holds";

    // R4 fix: a CLOSURE's nestedIndex must name its own target function, not
    // some other same-shaped node.
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op != Op::CLOSURE) {
            continue;
        }
        ASSERT_GE(ins.nestedIndex, 0) << "CLOSURE has no nestedIndex";
        ASSERT_LT(static_cast<size_t>(ins.nestedIndex), node.nested.size())
            << "CLOSURE nestedIndex is out of range";
        EXPECT_EQ(node.nested[static_cast<size_t>(ins.nestedIndex)].function,
                  asObjFunction(chunk.getConstant(
                      static_cast<uint16_t>(ins.constantIndex))))
            << "CLOSURE nestedIndex does not name its own target function";
    }

    for (const DecodedFunction& child : node.nested) {
        checkNode(child, mm, path + " > " + child.displayName);
    }
}

// Compiles `source`, decodes the whole ObjFunction tree, and checks every
// node against the oracle. Throws on a compile failure — a probe or example
// that fails to compile is a corpus problem, not a soft-fail case.
void checkSource(const std::string& source, const std::string& label) {
    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed for " + label);
    }
    DecodedFunction tree = decodeFunctionTree(script);
    checkNode(tree, mm, label);
}

void checkFile(const fs::path& path) {
    SCOPED_TRACE("file=" + path.string());
    checkSource(readFile(path), path.filename().string());
}

// Tallies one instruction's opcode into `counts`, then recurses into nested
// functions. Used only by the opcode-coverage test below.
void tallyOpCounts(const DecodedFunction& node, std::map<Op, int>& counts) {
    for (const DecodedInstruction& ins : node.instructions) {
        counts[ins.op]++;
    }
    for (const DecodedFunction& child : node.nested) {
        tallyOpCounts(child, counts);
    }
}

// Compiles and decodes `path`, then adds its opcodes to `counts`. Throws on a
// compile failure, matching checkSource's convention.
void accumulateOpCounts(const fs::path& path, std::map<Op, int>& counts) {
    MemoryManager mm;
    ObjFunction* script = compile(readFile(path), &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed for " + path.string());
    }
    tallyOpCounts(decodeFunctionTree(script), counts);
}

} // namespace

TEST(ChunkDecoderTest, MatchesOracleOnTranslationProbes) {
    std::vector<fs::path> probes =
        listLoxFiles(projectRoot() / "test" / "translation-probes");
    ASSERT_FALSE(probes.empty()) << "no translation probes found";
    for (const fs::path& probe : probes) {
        checkFile(probe);
    }
}

TEST(ChunkDecoderTest, MatchesOracleOnExamples) {
    std::vector<fs::path> examples = listLoxFiles(projectRoot() / "examples");
    ASSERT_FALSE(examples.empty()) << "no example programs found";
    for (const fs::path& example : examples) {
        checkFile(example);
    }
}

TEST(ChunkDecoderTest, MatchesOracleOnBootstrapInterpreter) {
    checkFile(projectRoot() / "bootstrap" / "loxpp_interpreter.lox");
}

TEST(ChunkDecoderTest, AssignsStableDeterministicIdentity) {
    // Two classes with same-named methods must not collide: identity is
    // structural (pool position), not name-based.
    MemoryManager mm;
    ObjFunction* script = compile(R"(
        class A { greet() { return 1; } }
        class B { greet() { return 2; } }
    )",
                                  &mm);
    ASSERT_NE(script, nullptr);
    DecodedFunction tree = decodeFunctionTree(script);
    ASSERT_EQ(tree.id, "0");
    ASSERT_EQ(tree.nested.size(), 2U);
    EXPECT_EQ(tree.nested[0].id, "0.0");
    EXPECT_EQ(tree.nested[1].id, "0.1");
    EXPECT_EQ(tree.nested[0].displayName, "greet");
    EXPECT_EQ(tree.nested[1].displayName, "greet");
    EXPECT_NE(tree.nested[0].function, tree.nested[1].function);
}

// R1 fix: a decoder bug in one opcode's width shifts every later instruction
// in the chunk to a wrong offset, but the three tests above cannot see that
// for an opcode the corpus never emits. This test closes that gap for good:
// it fails on any Op enumerator with a corpus-wide count of zero, for this
// opcode and for every opcode the language gains later.
TEST(ChunkDecoderTest, DecodesEveryOpcodeAtLeastOnce) {
    std::map<Op, int> counts;

    for (const fs::path& probe :
         listLoxFiles(projectRoot() / "test" / "translation-probes")) {
        accumulateOpCounts(probe, counts);
    }
    for (const fs::path& example : listLoxFiles(projectRoot() / "examples")) {
        accumulateOpCounts(example, counts);
    }
    accumulateOpCounts(projectRoot() / "bootstrap" / "loxpp_interpreter.lox",
                       counts);

    for (Op op : allOps()) {
        EXPECT_GT(counts[op], 0)
            << mnemonic(op) << " is never decoded anywhere in the corpus";
    }
}
