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
//   1. notes/translation-probes/*.lox — the probes the DAG analysis is
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

// Re-renders one decoded instruction into the exact text
// disassembleInstruction (src/debug.cpp) prints for it with color disabled.
// The category groups mirror decodeOne() in chunk_decoder.cpp; each mnemonic
// is generated from the Op enumerator's own spelling, since debug.cpp always
// uses that spelling verbatim.
const char* mnemonic(Op op) {
    switch (op) {
#define LOXPP_MNEMONIC(name)                                                   \
    case Op::name:                                                             \
        return #name;
        LOXPP_MNEMONIC(CONSTANT)
        LOXPP_MNEMONIC(NIL)
        LOXPP_MNEMONIC(TRUE)
        LOXPP_MNEMONIC(FALSE)
        LOXPP_MNEMONIC(EQUAL)
        LOXPP_MNEMONIC(GREATER)
        LOXPP_MNEMONIC(LESS)
        LOXPP_MNEMONIC(NEGATE)
        LOXPP_MNEMONIC(ADD)
        LOXPP_MNEMONIC(SUBTRACT)
        LOXPP_MNEMONIC(MULTIPLY)
        LOXPP_MNEMONIC(DIVIDE)
        LOXPP_MNEMONIC(MODULO)
        LOXPP_MNEMONIC(NOT)
        LOXPP_MNEMONIC(PRINT)
        LOXPP_MNEMONIC(POP)
        LOXPP_MNEMONIC(GET_LOCAL)
        LOXPP_MNEMONIC(SET_LOCAL)
        LOXPP_MNEMONIC(DEFINE_GLOBAL)
        LOXPP_MNEMONIC(GET_GLOBAL)
        LOXPP_MNEMONIC(SET_GLOBAL)
        LOXPP_MNEMONIC(JUMP)
        LOXPP_MNEMONIC(JUMP_IF_FALSE)
        LOXPP_MNEMONIC(LOOP)
        LOXPP_MNEMONIC(CALL)
        LOXPP_MNEMONIC(RETURN)
        LOXPP_MNEMONIC(CLOSURE)
        LOXPP_MNEMONIC(GET_UPVALUE)
        LOXPP_MNEMONIC(SET_UPVALUE)
        LOXPP_MNEMONIC(CLOSE_UPVALUE)
        LOXPP_MNEMONIC(CLASS)
        LOXPP_MNEMONIC(GET_PROPERTY)
        LOXPP_MNEMONIC(SET_PROPERTY)
        LOXPP_MNEMONIC(DEFINE_METHOD)
        LOXPP_MNEMONIC(INVOKE)
        LOXPP_MNEMONIC(INHERIT)
        LOXPP_MNEMONIC(GET_SUPER)
        LOXPP_MNEMONIC(SUPER_INVOKE)
        LOXPP_MNEMONIC(BUILD_LIST)
        LOXPP_MNEMONIC(BUILD_MAP)
        LOXPP_MNEMONIC(GET_INDEX)
        LOXPP_MNEMONIC(SET_INDEX)
        LOXPP_MNEMONIC(SLICE)
        LOXPP_MNEMONIC(IN)
        LOXPP_MNEMONIC(GET_ITER)
        LOXPP_MNEMONIC(ITER_HAS_NEXT)
        LOXPP_MNEMONIC(ITER_NEXT)
        LOXPP_MNEMONIC(MATCH_ERROR)
        LOXPP_MNEMONIC(JUMP_TABLE)
        LOXPP_MNEMONIC(GET_TAG)
        LOXPP_MNEMONIC(INSTANCEOF)
        LOXPP_MNEMONIC(IS_SEQ)
#undef LOXPP_MNEMONIC
    }
    return "UNKNOWN";
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

} // namespace

TEST(ChunkDecoderTest, MatchesOracleOnTranslationProbes) {
    std::vector<fs::path> probes =
        listLoxFiles(projectRoot() / "notes" / "translation-probes");
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
