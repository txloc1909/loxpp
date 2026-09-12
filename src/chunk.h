#pragma once

#include "value.h"

#include <cstdint>
#include <optional>
#include <vector>
#include <utility>

using Byte = uint8_t;

// clang-format off
enum class Op : Byte {
    CONSTANT,
    NIL,
    TRUE,
    FALSE,
    EQUAL,
    GREATER,
    LESS,
    NEGATE,
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
    MODULO,
    NOT,
    PRINT,
    POP,
    GET_LOCAL,
    SET_LOCAL,
    DEFINE_GLOBAL,
    GET_GLOBAL,
    SET_GLOBAL,
    JUMP,
    JUMP_IF_FALSE,
    LOOP,
    CALL,
    RETURN,
    CLOSURE,
    GET_UPVALUE,
    SET_UPVALUE,
    CLOSE_UPVALUE,
    CLASS,
    GET_PROPERTY,
    SET_PROPERTY,
    DEFINE_METHOD,
    INVOKE,    // operands: name-constant-index (2 bytes), arg-count (1 byte)
    INHERIT,   // no operand — copies superclass methods into subclass
    GET_SUPER, // 2-byte constant (method name) — binds method to 'this' from
               // superclass
    SUPER_INVOKE, // operands: name-constant-index (2 bytes), arg-count (1 byte)
    BUILD_LIST,   // operand: uint8 element count; pops N values, pushes ObjList
    BUILD_MAP,    // operand: uint8 pair count; pops 2*N values, pushes ObjMap
    GET_INDEX,    // pops index then list/string/map; pushes element/char/value
    SET_INDEX, // pops value, index, list/map; sets [index]=value; pushes value
    SLICE,     // pops end, start, seq; pushes new List|String slice
    IN,        // pops seq (rhs) then elem (lhs); pushes bool membership result
    GET_ITER,  // pops List|String|Map → pushes ObjIterator{cursor=0}
    ITER_HAS_NEXT, // pops iterator copy → pushes bool (cursor < length)
    ITER_NEXT, // pops iterator copy → pushes element/key at cursor, advances
    MATCH_ERROR, // no operands — raises MatchError; VM never returns
    // min_tag (1 byte), count (1 byte), then count×2 forward-offset bytes.
    // Pops the tag integer; if tag-min_tag is in [0,count) jumps to that arm's
    // code; otherwise falls through (to MATCH_ERROR for the out-of-range case).
    JUMP_TABLE,
    GET_TAG,     // no operands — pop ObjEnum, push ctor->tag as Number
    INSTANCEOF,  // 2-byte constant (class name ObjString*); pop value, push
                 // bool
    IS_SEQ, // no operands — pop value, push true if it is a sequence type

    // catchOffset (2-byte forward-relative offset, encoded exactly like
    // JUMP's — see chunk_decoder.cpp). Pushes a handler record (checkpoint
    // stack depth, catchOffset) onto the VM's separate handler stack; no
    // operand-stack effect of its own.
    //
    // Despite the JUMP-shaped operand, PUSH_HANDLER is not a branch: control
    // always falls through to the protected code that follows it, and the
    // catch-handler entry named by catchOffset is reached only through a
    // THROW unwind, never by falling or jumping there directly. A catch
    // handler's real predecessors are every THROW reachable in the
    // protected region — including ones in a callee this function's own
    // bytecode cannot see — so cfg.cpp/abstract_stack.cpp must never treat
    // catchOffset as an ordinary jump target: the catch entry's operand
    // depth is a *declared* contract (checkpoint depth + 1, for the thrown
    // value), not something discovered via predecessor agreement. See
    // notes/non-local-control-flow.md and notes/bytecode-translation-
    // problems.md.
    PUSH_HANDLER,
    POP_HANDLER, // no operands — pops the current handler record
    // no operands — pops the value to raise, unwinds to the nearest
    // PUSH_HANDLER checkpoint. Terminal: like RETURN, control never falls
    // through past THROW into the next instruction in this function.
    THROW,
};
// clang-format on

// Single source of truth for "every Op enumerator, spelled as itself".
// test/test_chunk_decoder.cpp's decoder oracle and src/profiler.h's opcode
// report both name-switch over Op; both are driven by this macro so a new
// enumerator only needs adding here once, next to the enum it describes,
// instead of in each hand-typed switch separately.
#define LOXPP_FOR_EACH_OP(X)                                                   \
    X(CONSTANT)                                                                \
    X(NIL)                                                                     \
    X(TRUE)                                                                    \
    X(FALSE)                                                                   \
    X(EQUAL)                                                                   \
    X(GREATER)                                                                 \
    X(LESS)                                                                    \
    X(NEGATE)                                                                  \
    X(ADD)                                                                     \
    X(SUBTRACT)                                                                \
    X(MULTIPLY)                                                                \
    X(DIVIDE)                                                                  \
    X(MODULO)                                                                  \
    X(NOT)                                                                     \
    X(PRINT)                                                                   \
    X(POP)                                                                     \
    X(GET_LOCAL)                                                               \
    X(SET_LOCAL)                                                               \
    X(DEFINE_GLOBAL)                                                           \
    X(GET_GLOBAL)                                                              \
    X(SET_GLOBAL)                                                              \
    X(JUMP)                                                                    \
    X(JUMP_IF_FALSE)                                                           \
    X(LOOP)                                                                    \
    X(CALL)                                                                    \
    X(RETURN)                                                                  \
    X(CLOSURE)                                                                 \
    X(GET_UPVALUE)                                                             \
    X(SET_UPVALUE)                                                             \
    X(CLOSE_UPVALUE)                                                           \
    X(CLASS)                                                                   \
    X(GET_PROPERTY)                                                            \
    X(SET_PROPERTY)                                                            \
    X(DEFINE_METHOD)                                                           \
    X(INVOKE)                                                                  \
    X(INHERIT)                                                                 \
    X(GET_SUPER)                                                               \
    X(SUPER_INVOKE)                                                            \
    X(BUILD_LIST)                                                              \
    X(BUILD_MAP)                                                               \
    X(GET_INDEX)                                                               \
    X(SET_INDEX)                                                               \
    X(SLICE)                                                                   \
    X(IN)                                                                      \
    X(GET_ITER)                                                                \
    X(ITER_HAS_NEXT)                                                           \
    X(ITER_NEXT)                                                               \
    X(MATCH_ERROR)                                                             \
    X(JUMP_TABLE)                                                              \
    X(GET_TAG)                                                                 \
    X(INSTANCEOF)                                                              \
    X(IS_SEQ)                                                                  \
    X(PUSH_HANDLER)                                                            \
    X(POP_HANDLER)                                                             \
    X(THROW)

inline Op toOpcode(Byte byte) { return static_cast<Op>(byte); }

// Name of an Op enumerator, e.g. opcodeName(Op::ADD) == "ADD". Falls back to
// "UNKNOWN" for a byte that does not decode to a known Op (corrupt bytecode).
const char* opcodeName(Op op);

class Chunk : std::vector<Byte> {
  public:
    using std::vector<Byte>::at;
    using std::vector<Byte>::data;
    using std::vector<Byte>::const_iterator;
    using std::vector<Byte>::cbegin;
    using std::vector<Byte>::cend;
    using std::vector<Byte>::size;

    void write(Byte byte, int line);
    void write(Op op, int line);
    void patch(int offset, Byte byte);
    std::optional<uint16_t> addConstant(Value value);
    [[nodiscard]] Value getConstant(uint16_t idx) const;
    [[nodiscard]] const ValueArray& constants() const { return m_constants; }
    [[nodiscard]] int getLine(int offset) const;

  private:
    ValueArray m_constants;
    std::vector<std::pair<int, int>> m_lines;
};
