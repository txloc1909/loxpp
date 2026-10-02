// qbe_emit_probe.cpp — S3's own checkpoint driver (#456,
// notes/qbe-backend.md). Compiles one Lox++ source file, prints its QBE
// .ssa text (backend/qbe_emitter.h) to stdout, and the RtFunctionDesc
// triple (id, arity, chunkHash — backend/rt_capi.h) the checkpoint's
// harness needs to attach the assembled code, as one space-separated line
// on stderr. Rejects (exit 70) a program that declares any function or
// closure: CALL/CLOSURE lowering is S4's job (#457), and this tool exists
// only to drive S3's own straight-line/jump checkpoint
// (tools/check_qbe_s3_straight_line.sh), not to be a general --target qbe
// front-end — that CLI wiring is a later node's job.

#include "backend/capture_analysis.h"
#include "backend/chunk_decoder.h"
#include "backend/qbe_emitter.h"
#include "compiler.h"
#include "exec_objects.h"
#include "memory_manager.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: qbe_emit_probe program.lox qbe-symbol\n");
        return 64;
    }
    std::ifstream file(argv[1]);
    if (!file) {
        std::fprintf(stderr, "qbe_emit_probe: cannot read %s\n", argv[1]);
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
    if (!tree.nested.empty()) {
        std::fprintf(stderr,
                     "qbe_emit_probe: %s declares a function or closure — "
                     "S4's job (#457), not S3's straight-line/jump "
                     "checkpoint\n",
                     argv[1]);
        return 70;
    }

    try {
        FunctionStackAnalysis analysis = analyzeStack(tree);
        FunctionCaptureInfo captures =
            analyzeCaptures(tree).functions.at(tree.id);
        std::string ssa = qbe::emitScript(tree, analysis, captures, argv[2]);
        std::fputs(ssa.c_str(), stdout);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "qbe_emit_probe: %s\n", e.what());
        return 70;
    }

    std::fprintf(
        stderr, "%s %d %llu\n", tree.id.c_str(), script->arity,
        static_cast<unsigned long long>(hashChunkBytes(script->chunk)));
    return 0;
}
