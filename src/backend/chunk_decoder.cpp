#include "chunk_decoder.h"
#include "exec_objects.h"
#include "object.h"
#include "value.h"

#include <stdexcept>
#include <unordered_map>

namespace {

// Big-endian 16-bit read, matching every 2-byte operand in debug.cpp.
uint16_t readU16(const Chunk& chunk, int offset) {
    return static_cast<uint16_t>(chunk.at(offset) << 8 | chunk.at(offset + 1));
}

std::string functionDisplayName(ObjFunction* fn) {
    return fn->name != nullptr ? std::string(fn->name->chars) : "script";
}

// Decodes one instruction at `offset`. Returns the decoded instruction; the
// caller advances by its `length`.
DecodedInstruction decodeOne(const Chunk& chunk, int offset) {
    DecodedInstruction ins;
    ins.offset = offset;
    ins.op = toOpcode(chunk.at(offset));

    switch (ins.op) {
    // No operand.
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
        ins.length = 1;
        break;

    // 2-byte constant-pool index.
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
        ins.constantIndex = readU16(chunk, offset + 1);
        ins.length = 3;
        break;

    // 1-byte operand.
    case Op::GET_LOCAL:
    case Op::SET_LOCAL:
    case Op::CALL:
    case Op::BUILD_LIST:
    case Op::BUILD_MAP:
    case Op::GET_UPVALUE:
    case Op::SET_UPVALUE:
        ins.byteOperand = chunk.at(offset + 1);
        ins.length = 2;
        break;

    case Op::JUMP:
    case Op::JUMP_IF_FALSE: {
        uint16_t jump = readU16(chunk, offset + 1);
        ins.length = 3;
        ins.jumpTarget = offset + ins.length + static_cast<int>(jump);
        break;
    }
    case Op::LOOP: {
        uint16_t jump = readU16(chunk, offset + 1);
        ins.length = 3;
        ins.jumpTarget = offset + ins.length - static_cast<int>(jump);
        break;
    }

    // 2-byte name constant, then 1-byte arg count.
    case Op::INVOKE:
    case Op::SUPER_INVOKE:
        ins.constantIndex = readU16(chunk, offset + 1);
        ins.byteOperand = chunk.at(offset + 3);
        ins.length = 4;
        break;

    // 2-byte function constant, then 2 * upvalueCount trailing bytes — the
    // count lives on the *target* function, not in the instruction stream
    // (P4b in bytecode-translation-problems.md).
    case Op::CLOSURE: {
        uint16_t idx = readU16(chunk, offset + 1);
        ins.constantIndex = idx;
        Value fnVal = chunk.getConstant(idx);
        if (!is<Obj*>(fnVal) ||
            !isObjType(as<Obj*>(fnVal), ObjType::FUNCTION)) {
            throw std::runtime_error("chunk_decoder: CLOSURE at offset " +
                                     std::to_string(offset) +
                                     " does not name a function constant");
        }
        ObjFunction* target = asObjFunction(fnVal);
        int cursor = offset + 3;
        for (int i = 0; i < target->upvalueCount; i++) {
            bool isLocal = chunk.at(cursor) != 0;
            uint8_t index = chunk.at(cursor + 1);
            ins.upvalues.push_back({isLocal, index});
            cursor += 2;
        }
        ins.length = cursor - offset;
        break;
    }

    // min_tag (1 byte), count (1 byte), then count * 2 forward-offset bytes.
    case Op::JUMP_TABLE: {
        uint8_t minTag = chunk.at(offset + 1);
        uint8_t count = chunk.at(offset + 2);
        int tableEnd = offset + 3 + count * 2;
        ins.minTag = minTag;
        for (int i = 0; i < static_cast<int>(count); i++) {
            uint16_t fwd = readU16(chunk, offset + 3 + i * 2);
            ins.jumpTable.push_back(
                {minTag + i, tableEnd + static_cast<int>(fwd)});
        }
        ins.length = tableEnd - offset;
        break;
    }

    default:
        throw std::runtime_error(
            "chunk_decoder: unknown opcode " +
            std::to_string(static_cast<unsigned>(chunk.at(offset))) +
            " at offset " + std::to_string(offset));
    }

    return ins;
}

} // namespace

std::vector<DecodedInstruction> decodeChunk(const Chunk& chunk) {
    std::vector<DecodedInstruction> result;
    int size = static_cast<int>(chunk.size());
    int offset = 0;
    while (offset < size) {
        DecodedInstruction ins = decodeOne(chunk, offset);
        offset += ins.length;
        result.push_back(std::move(ins));
    }
    return result;
}

namespace {

DecodedFunction decodeFunctionNode(ObjFunction* fn, std::string id) {
    DecodedFunction node;
    node.function = fn;
    node.displayName = functionDisplayName(fn);
    node.instructions = decodeChunk(fn->chunk);

    // constant-pool index -> position in node.nested. CLOSURE names its
    // target by constant-pool index (below); `nested` is ordered by
    // function-constant position instead, so this map links the two.
    std::unordered_map<int, int> nestedIndexOf;
    const ValueArray& constants = fn->chunk.constants();
    int childIndex = 0;
    for (uint16_t i = 0; i < constants.size(); i++) {
        Value v = constants.at(i);
        if (is<Obj*>(v) && isObjType(as<Obj*>(v), ObjType::FUNCTION)) {
            nestedIndexOf[i] = childIndex;
            node.nested.push_back(decodeFunctionNode(
                asObjFunction(v), id + "." + std::to_string(childIndex)));
            childIndex++;
        }
    }

    for (DecodedInstruction& ins : node.instructions) {
        if (ins.op == Op::CLOSURE) {
            ins.nestedIndex = nestedIndexOf.at(ins.constantIndex);
        }
    }

    node.id = std::move(id);
    return node;
}

} // namespace

DecodedFunction decodeFunctionTree(ObjFunction* root) {
    return decodeFunctionNode(root, "0");
}
