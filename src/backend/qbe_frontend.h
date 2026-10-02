#pragma once

#include <string>

// Compiles `scriptPath` with the QBE backend into a standalone native
// executable at `exePath` (the pipeline in notes/qbe-backend.md): emit one
// .ssa translation unit for the whole function tree, generate a harness that
// embeds the source and the function descriptors, run `qbe`, then link the
// result against libloxrt.a. The produced binary carries its own source, so
// it does not need the .lox file at run time.
//
// Tool discovery: `QBE` and `CXX` override the `qbe` and `c++`/`cc` names;
// `LOX_RT_A` overrides libloxrt.a, which is otherwise looked for beside the
// loxpp binary.
//
// `noPromote` and `noFuse` disable S8's register promotion and
// GET_TAG;JUMP_TABLE fusion respectively, so a benchmark can compare the
// pre-S8 emitter against the default one in a single build.
//
// Exit codes match the rest of the CLI: 0 success, 65 compile error, 70 an
// emitter or external-tool failure, 74 a file-system failure or a missing
// libloxrt.a.
int runQbeTarget(const std::string& exePath, const std::string& scriptPath,
                 bool noPromote, bool noFuse);
