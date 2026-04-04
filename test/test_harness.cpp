#include "test_harness.h"
#include "vm.h"
#include "compiler.h"
#include "debug.h"
#include "simple_allocator.h"
#include <sstream>

Value eval_expr(const std::string& expr) {
    VM vm;
    if (vm.interpret(expr + ";") != InterpretResult::OK) {
        throw std::runtime_error("Interpretation failed");
    }
    return vm.lastResult();
}

std::string eval_expr_str(const std::string& expr) {
    VM vm;
    if (vm.interpret(expr + ";") != InterpretResult::OK) {
        throw std::runtime_error("Interpretation failed");
    }
    return stringify(vm.lastResult(), vm.allocator());
}

std::string compile_to_bytecode(const std::string& expr) {
    SimpleAllocator allocator;
    auto chunk = compile(expr + ";", &allocator);
    if (!chunk)
        throw std::runtime_error("Compilation failed");
    std::ostringstream oss;
    for (int offset = 0; offset < static_cast<int>(chunk->size());) {
        offset = disassembleInstruction(*chunk, allocator, offset, oss);
    }
    return oss.str();
}
