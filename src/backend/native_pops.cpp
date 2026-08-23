#include "native_pops.h"

#include <stdexcept>
#include <string>

std::string opName(Op op) {
    switch (op) {
    case Op::CONSTANT:
        return "CONSTANT";
    case Op::NIL:
        return "NIL";
    case Op::TRUE:
        return "TRUE";
    case Op::FALSE:
        return "FALSE";
    case Op::EQUAL:
        return "EQUAL";
    case Op::GREATER:
        return "GREATER";
    case Op::LESS:
        return "LESS";
    case Op::NEGATE:
        return "NEGATE";
    case Op::ADD:
        return "ADD";
    case Op::SUBTRACT:
        return "SUBTRACT";
    case Op::MULTIPLY:
        return "MULTIPLY";
    case Op::DIVIDE:
        return "DIVIDE";
    case Op::MODULO:
        return "MODULO";
    case Op::NOT:
        return "NOT";
    case Op::PRINT:
        return "PRINT";
    case Op::POP:
        return "POP";
    case Op::GET_LOCAL:
        return "GET_LOCAL";
    case Op::SET_LOCAL:
        return "SET_LOCAL";
    case Op::DEFINE_GLOBAL:
        return "DEFINE_GLOBAL";
    case Op::GET_GLOBAL:
        return "GET_GLOBAL";
    case Op::SET_GLOBAL:
        return "SET_GLOBAL";
    case Op::JUMP:
        return "JUMP";
    case Op::JUMP_IF_FALSE:
        return "JUMP_IF_FALSE";
    case Op::LOOP:
        return "LOOP";
    case Op::CALL:
        return "CALL";
    case Op::RETURN:
        return "RETURN";
    case Op::CLOSURE:
        return "CLOSURE";
    case Op::GET_UPVALUE:
        return "GET_UPVALUE";
    case Op::SET_UPVALUE:
        return "SET_UPVALUE";
    case Op::CLOSE_UPVALUE:
        return "CLOSE_UPVALUE";
    case Op::CLASS:
        return "CLASS";
    case Op::GET_PROPERTY:
        return "GET_PROPERTY";
    case Op::SET_PROPERTY:
        return "SET_PROPERTY";
    case Op::DEFINE_METHOD:
        return "DEFINE_METHOD";
    case Op::INVOKE:
        return "INVOKE";
    case Op::INHERIT:
        return "INHERIT";
    case Op::GET_SUPER:
        return "GET_SUPER";
    case Op::SUPER_INVOKE:
        return "SUPER_INVOKE";
    case Op::BUILD_LIST:
        return "BUILD_LIST";
    case Op::BUILD_MAP:
        return "BUILD_MAP";
    case Op::GET_INDEX:
        return "GET_INDEX";
    case Op::SET_INDEX:
        return "SET_INDEX";
    case Op::SLICE:
        return "SLICE";
    case Op::IN:
        return "IN";
    case Op::GET_ITER:
        return "GET_ITER";
    case Op::ITER_HAS_NEXT:
        return "ITER_HAS_NEXT";
    case Op::ITER_NEXT:
        return "ITER_NEXT";
    case Op::MATCH_ERROR:
        return "MATCH_ERROR";
    case Op::JUMP_TABLE:
        return "JUMP_TABLE";
    case Op::GET_TAG:
        return "GET_TAG";
    case Op::INSTANCEOF:
        return "INSTANCEOF";
    case Op::IS_SEQ:
        return "IS_SEQ";
    }
    return "UNKNOWN_OP";
}

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
    throw std::runtime_error("nativePops has no row for " + opName(op));
}
