// s3_harness.cpp — S3's own checkpoint driver (#456,
// tools/check_qbe_s3_straight_line.sh). One fixed, reusable main(): reads a
// Lox++ source file (the same probe qbe_emit_probe compiled), starts a
// Runtime over it (rt_startup recompiles that same source and attaches the
// already-assembled QBE code by id/arity/chunkHash), then runs the script
// exactly the way a real --target qbe program's own entry point would —
// loadSource() already left the script closure on the stack at slot 0
// (runtime.cpp), so rt_call(rt, 0) is the whole driver.
//
// `lox_fn_0` is the fixed QBE symbol name every probe is compiled under
// (qbe_emit_probe's own second argument, always "lox_fn_0" for this
// checkpoint) — declared here as the RtCompiledFn the linker resolves
// against the probe's own assembled .s file.

#include "backend/rt_abi.h"
#include "backend/rt_capi.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

extern "C" int lox_fn_0(Runtime*, Value*);

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: s3_harness program.lox id arity chunk-hash\n");
        return 64;
    }
    std::ifstream file(argv[1]);
    if (!file) {
        std::fprintf(stderr, "s3_harness: cannot read %s\n", argv[1]);
        return 74;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    RtFunctionDesc desc{};
    desc.id = argv[2];
    desc.arity = std::atoi(argv[3]);
    desc.chunkHash = std::strtoull(argv[4], nullptr, 10);
    desc.code = reinterpret_cast<void*>(&lox_fn_0);

    Runtime* rt = rt_startup(source.c_str(), &desc, 1);
    if (rt == nullptr) {
        return 65;
    }
    int status = rt_call(rt, 0);
    rt_shutdown(rt);
    return status == 0 ? 0 : 70;
}
