#include "test_harness.h"
#include "vm.h"
#include "compiler.h"
#include "debug.h"
#include "memory_manager.h"
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
    return stringify(vm.lastResult());
}

std::string compile_to_bytecode(const std::string& expr) {
    MemoryManager mm;
    auto chunk = compile(expr + ";", &mm);
    if (!chunk)
        throw std::runtime_error("Compilation failed");
    std::ostringstream oss;
    for (int offset = 0; offset < static_cast<int>(chunk->size());) {
        offset = disassembleInstruction(*chunk, mm, offset, oss);
    }
    return oss.str();
}

std::string compile_program_to_bytecode(const std::string& source) {
    MemoryManager mm;
    auto chunk = compile(source, &mm);
    if (!chunk)
        throw std::runtime_error("Compilation failed");
    std::ostringstream oss;
    for (int offset = 0; offset < static_cast<int>(chunk->size());) {
        offset = disassembleInstruction(*chunk, mm, offset, oss);
    }
    return oss.str();
}

InterpretResult run_program(const std::string& source) {
    VM vm;
    return vm.interpret(source);
}

// ---------------------------------------------------------------------------
// VMTestHarness
// ---------------------------------------------------------------------------

InterpretResult VMTestHarness::run(const std::string& source) {
    return m_vm.interpret(source);
}

std::string VMTestHarness::getGlobalStr(const std::string& name) const {
    auto v = m_vm.getGlobal(name);
    if (!v)
        return "<undefined>";
    return stringify(*v);
}
