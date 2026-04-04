#include "test_harness.h"
#include "vm.h"
#include "compiler.h"
#include "simple_allocator.h"
#include <sstream>

Value eval_expr(const std::string& expr) {
    VM vm;
    if (vm.interpret(expr) != InterpretResult::OK) {
        throw std::runtime_error("Interpretation failed");
    }
    return vm.lastResult();
}

std::string eval_expr_str(const std::string& expr) {
    VM vm;
    if (vm.interpret(expr) != InterpretResult::OK) {
        throw std::runtime_error("Interpretation failed");
    }
    return stringify(vm.lastResult(), vm.allocator());
}

std::string compile_to_bytecode(const std::string& expr) {
    SimpleAllocator allocator;
    auto chunk = compile(expr, &allocator);
    if (!chunk)
        throw std::runtime_error("Compilation failed");
    std::ostringstream oss;
    // TODO: implement a proper disassembler and reuse it here instead of this
    // ad-hoc switch.
    for (size_t offset = 0; offset < chunk->size();) {
        Op op = toOpcode(chunk->at(offset));
        oss << offset << ": ";
        switch (op) {
        case Op::CONSTANT: {
            uint8_t idx = chunk->at(offset + 1);
            Value v = chunk->getConstant(idx);
            oss << "CONSTANT " << (int)idx << " ('" << stringify(v, allocator)
                << "')\n";
            offset += 2;
            break;
        }
        case Op::NIL:
            oss << "NIL\n";
            offset += 1;
            break;
        case Op::TRUE:
            oss << "TRUE\n";
            offset += 1;
            break;
        case Op::FALSE:
            oss << "FALSE\n";
            offset += 1;
            break;
        case Op::EQUAL:
            oss << "EQUAL\n";
            offset += 1;
            break;
        case Op::GREATER:
            oss << "GREATER\n";
            offset += 1;
            break;
        case Op::LESS:
            oss << "LESS\n";
            offset += 1;
            break;
        case Op::NEGATE:
            oss << "NEGATE\n";
            offset += 1;
            break;
        case Op::ADD:
            oss << "ADD\n";
            offset += 1;
            break;
        case Op::SUBTRACT:
            oss << "SUBTRACT\n";
            offset += 1;
            break;
        case Op::MULTIPLY:
            oss << "MULTIPLY\n";
            offset += 1;
            break;
        case Op::DIVIDE:
            oss << "DIVIDE\n";
            offset += 1;
            break;
        case Op::NOT:
            oss << "NOT\n";
            offset += 1;
            break;
        case Op::RETURN:
            oss << "RETURN\n";
            offset += 1;
            break;
        default:
            oss << "UNKNOWN\n";
            offset += 1;
            break;
        }
    }
    return oss.str();
}
