#pragma once

// Target-independent bytecode decoder. It turns a Chunk's raw byte stream
// into structured instructions, and walks the ObjFunction tree those chunks
// belong to. It carries no JVM or CLR knowledge — both backends consume it.
//
// This is a decode pass only. It does not compute a CFG (cfg.h), an
// abstract stack (abstract_stack.h), or a capture map (capture_analysis.h);
// those passes consume the instructions decoded here.

#include "chunk.h"

#include <cstdint>
#include <string>
#include <vector>

struct ObjFunction;

// One captured upvalue named by a CLOSURE operand. `isLocal` selects whether
// `index` names a slot in the enclosing function (true) or an upvalue slot
// the enclosing function already captured (false) — see vm.cpp's CLOSURE
// handler.
struct ClosureUpvalue {
    bool isLocal;
    uint8_t index;
};

// One arm of a JUMP_TABLE. `tag` is the enum tag value that selects this
// arm; `target` is the absolute byte offset of its code.
struct JumpTableArm {
    int tag;
    int target;
};

// A single decoded instruction. Only the fields that apply to `op` are set;
// the rest keep their default. See chunk_decoder.cpp for which opcode
// populates which field — it mirrors src/debug.cpp exactly.
struct DecodedInstruction {
    int offset{0};
    Op op{};
    int length{0};

    // Constant-pool index. Set by CONSTANT, DEFINE_GLOBAL, GET_GLOBAL,
    // SET_GLOBAL, CLASS, GET_PROPERTY, SET_PROPERTY, DEFINE_METHOD,
    // GET_SUPER, INSTANCEOF, CLOSURE, and the name half of INVOKE /
    // SUPER_INVOKE.
    int constantIndex{-1};

    // A single trailing byte. Set by GET_LOCAL, SET_LOCAL, GET_UPVALUE,
    // SET_UPVALUE, CALL, BUILD_LIST, BUILD_MAP, and the arg-count half of
    // INVOKE / SUPER_INVOKE.
    int byteOperand{-1};

    // Absolute target offset. Set by JUMP, JUMP_IF_FALSE, LOOP.
    int jumpTarget{-1};

    // Set by CLOSURE: one entry per upvalue the target function captures,
    // in operand order.
    std::vector<ClosureUpvalue> upvalues;

    // Set by JUMP_TABLE: the switch base, plus one arm per tag in range.
    int minTag{-1};
    std::vector<JumpTableArm> jumpTable;

    // Set by CLOSURE: the position of the target function inside the
    // enclosing DecodedFunction::nested. `nested` is ordered by
    // function-constant position, not by constant-pool index, so this saves
    // every consumer from re-deriving the mapping by scanning `nested` for
    // the constant `constantIndex` names.
    int nestedIndex{-1};
};

// Decodes every instruction in `chunk`, in offset order. Throws
// std::runtime_error on an unrecognised opcode byte or a malformed CLOSURE
// constant — the compiler never emits either, so hitting one means the
// decoder and the compiler have drifted apart.
std::vector<DecodedInstruction> decodeChunk(const Chunk& chunk);

// One node of the decoded ObjFunction tree: a chunk's decoded instructions
// plus the nested functions found in its constant pool (every nested
// function or method is compiled to a FUNCTION-typed constant — see
// Compiler::funDeclaration / Compiler::method in compiler.cpp).
struct DecodedFunction {
    ObjFunction* function{nullptr};

    // Stable identity, independent of names: "0" for the root, and
    // "<parent-id>.<n>" for the n-th function constant (0-indexed, pool
    // order) found inside the parent's chunk. Two same-named methods on
    // different classes still get distinct ids.
    std::string id;

    // The header disassembleChunk prints for this chunk: the function's own
    // name, or "script" for the top-level chunk (compiler.cpp,
    // Compiler::endCompiler).
    std::string displayName;

    std::vector<DecodedInstruction> instructions;
    std::vector<DecodedFunction> nested;
};

// Walks the whole tree reachable from `root` — the top-level script
// function — decoding every chunk it finds.
DecodedFunction decodeFunctionTree(ObjFunction* root);
