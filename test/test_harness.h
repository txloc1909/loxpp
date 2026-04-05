#pragma once
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include "value.h"
#include "chunk.h"
#include "vm.h"

// ---------------------------------------------------------------------------
// Free functions (legacy harness — single expression)
// ---------------------------------------------------------------------------

// Evaluates a Lox++ expression and returns the result as a Value.
Value eval_expr(const std::string& expr);

// Evaluates a Lox++ expression and returns the result as a string (for easy
// comparison in tests).
std::string eval_expr_str(const std::string& expr);

// Compiles a Lox++ expression and returns a readable string representation of
// the bytecode chunk.
std::string compile_to_bytecode(const std::string& expr);

// Compiles a complete Lox++ program (no auto-semicolon) and returns a readable
// string representation of the bytecode chunk.
std::string compile_program_to_bytecode(const std::string& source);

// Runs a complete Lox++ program (no auto-semicolon) and returns the interpret
// result. Use for multi-statement programs where only pass/fail matters.
InterpretResult run_program(const std::string& source);

// ---------------------------------------------------------------------------
// VMTestHarness — wraps VM and exposes runtime state for invariant testing
// ---------------------------------------------------------------------------

// VMTestHarness is a thin wrapper around VM that exposes runtime state for
// testing. One harness per test case — state is fully isolated.
//
// Invariants this harness can verify:
//   1. stackDepth() == 0 after every complete, well-formed program.
//   2. getGlobal(name) returns std::nullopt for undefined variables.
//   3. getGlobal(name) returns the stored Value after DEFINE/SET_GLOBAL.
class VMTestHarness {
  public:
    VMTestHarness() = default;

    // Run a complete Lox++ program and return the interpret result.
    InterpretResult run(const std::string& source);

    // Stack depth at the moment of the call.
    int stackDepth() const { return m_vm.stackDepth(); }

    // Look up a global variable by name. Returns std::nullopt if the variable
    // was never defined (absent != nil).
    std::optional<Value> getGlobal(const std::string& name) const {
        return m_vm.getGlobal(name);
    }

    // The value stored by the most recent Op::POP (expression statement
    // result).
    Value lastResult() const { return m_vm.lastResult(); }

    // Convenience: get the string representation of a global variable's value.
    std::string getGlobalStr(const std::string& name) const;

  private:
    VM m_vm;
};
