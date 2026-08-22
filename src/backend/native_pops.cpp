#include "native_pops.h"

#include <stdexcept>
#include <string>

std::optional<int> nativePops(Op op, const DecodedInstruction& in) {
    switch (op) {
    // Push-only, or a control-flow op with nothing of its own to net-pop.
    case Op::CONSTANT:
    case Op::NIL:
    case Op::TRUE:
    case Op::FALSE:
    case Op::GET_LOCAL:
    case Op::GET_GLOBAL:
    case Op::GET_UPVALUE:
    case Op::CLASS:
    case Op::CLOSURE:
    case Op::MATCH_ERROR:
        return 0;
    // One operand read.
    case Op::NEGATE:
    case Op::NOT:
    case Op::PRINT:
    case Op::DEFINE_GLOBAL:
    case Op::GET_PROPERTY:
    case Op::GET_TAG:
    case Op::INSTANCEOF:
    case Op::IS_SEQ:
    case Op::RETURN:
    case Op::ITER_HAS_NEXT:
    case Op::ITER_NEXT:
        return 1;
    // Two operands read.
    case Op::EQUAL:
    case Op::GREATER:
    case Op::LESS:
    case Op::ADD:
    case Op::SUBTRACT:
    case Op::MULTIPLY:
    case Op::DIVIDE:
    case Op::MODULO:
    case Op::GET_INDEX:
    case Op::IN:
    case Op::SET_PROPERTY:  // reads the instance AND the value; pops both,
                            // pushes the value back
    case Op::DEFINE_METHOD: // reads the class AND the closure (`peek(1)`,
                            // `peek(0)`); pops only the closure, class stays
    case Op::GET_SUPER:     // pops the superclass AND `this` (bindMethod's own
                            // pop); never foldable
        return 2;
    // Three operands read.
    case Op::SET_INDEX:
    case Op::SLICE:
        return 3;
    // Width carried in the instruction's own operand byte.
    case Op::BUILD_LIST:
        return in.byteOperand;
    case Op::BUILD_MAP:
        return 2 * in.byteOperand;
    case Op::CALL:
    case Op::INVOKE: // receiver/callee plus argCount arguments
        return in.byteOperand + 1;
    case Op::SUPER_INVOKE: // self, superclass, plus argCount arguments —
        return in.byteOperand + 2; // self/superclass are never foldable
    // CUSTOM: the peek/locals-model family, a pure reclaim, an instruction
    // that already threads its own zero-depth load unconditionally, or pure
    // control transfer. See this function's own header note.
    case Op::POP:
    case Op::CLOSE_UPVALUE:
    case Op::SET_LOCAL:
    case Op::SET_GLOBAL:
    case Op::SET_UPVALUE:
    case Op::JUMP_IF_FALSE:
    case Op::GET_ITER:
    case Op::INHERIT:
    case Op::JUMP:
    case Op::LOOP:
    case Op::JUMP_TABLE:
        return std::nullopt;
    }
    // No `default:` above on purpose (this function's own header note) —
    // reachable only if `-Wswitch` was ignored, which is itself the bug to
    // fix.
    throw std::runtime_error("nativePops has no row for opcode " +
                             std::to_string(static_cast<int>(op)));
}
