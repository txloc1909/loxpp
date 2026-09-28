// qbe_emit_program.cpp — S4's own checkpoint driver (#457,
// notes/qbe-backend.md). Unlike S3's qbe_emit_probe (one function, no
// CLOSURE/CALL allowed), this tool compiles a WHOLE Lox++ program — the
// top-level script plus every function nested inside it, however deep —
// because CLOSURE/CALL mean a real program is rarely just one function.
//
// Emits, for every function in the tree, depth-first:
//   - stdout: that function's own QBE .ssa text (backend/qbe_emitter.h),
//     concatenated across the whole tree into one translation unit. Every
//     function's own emitted text is self-contained — CALL/CLOSURE reach
//     other functions only through the runtime (rt_call, rt_constant_at),
//     never by referencing another function's QBE symbol directly — so
//     concatenation needs no forward-declaration ordering.
//   - stderr: one line "<id> <arity> <chunkHash> <qbeSymbol>" — the
//     RtFunctionDesc triple (backend/rt_capi.h) plus the QBE symbol name a
//     harness needs to reference the compiled function once assembled and
//     linked.
//
// Each function's QBE symbol is "lox_fn_" + its id with every '.' replaced
// by '_' (QBE symbol names cannot hold '.') — stable and collision-free,
// since DecodedFunction::id already is (chunk_decoder.h).

#include "backend/abstract_stack.h"
#include "backend/chunk_decoder.h"
#include "backend/qbe_emitter.h"
#include "compiler.h"
#include "exec_objects.h"
#include "memory_manager.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string qbeSymbolFor(const std::string& id) {
    std::string sym = "lox_fn_" + id;
    for (char& c : sym) {
        if (c == '.') {
            c = '_';
        }
    }
    return sym;
}

void emitTree(const DecodedFunction& node,
              const StackAnalysisTree& analysisNode, std::FILE* out,
              std::FILE* err) {
    std::string symbol = qbeSymbolFor(node.id);
    std::string ssa = qbe::emitScript(node, analysisNode.self, symbol);
    std::fputs(ssa.c_str(), out);
    std::fprintf(
        err, "%s %d %llu %s\n", node.id.c_str(), node.function->arity,
        static_cast<unsigned long long>(hashChunkBytes(node.function->chunk)),
        symbol.c_str());
    for (std::size_t i = 0; i < node.nested.size(); i++) {
        emitTree(node.nested[i], analysisNode.nested[i], out, err);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: qbe_emit_program program.lox\n");
        return 64;
    }
    std::ifstream file(argv[1]);
    if (!file) {
        std::fprintf(stderr, "qbe_emit_program: cannot read %s\n", argv[1]);
        return 74;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        return 65;
    }

    DecodedFunction tree = decodeFunctionTree(script);
    try {
        StackAnalysisTree analysis = analyzeStackTree(tree);
        emitTree(tree, analysis, stdout, stderr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "qbe_emit_program: %s\n", e.what());
        return 70;
    }
    return 0;
}
