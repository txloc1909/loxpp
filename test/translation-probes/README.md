# Translation probes

Small Lox++ programs, each chosen so that its *bytecode* isolates one
translation problem for the JVM/QBE backends. They are the empirical ground for
`docs/reference/bytecode-translation-problems.md` — read that document alongside these.

The `Vn_*` probes are **verification** programs: their runtime *output* (not
just their bytecode) settles a semantic question the disassembly alone leaves
open — chiefly whether `CLOSE_UPVALUE` can be treated as a no-op (it cannot).

## Regenerating the bytecode

The stock build hides disassembly. Build a debug-print binary (clang required —
GCC rejects the parser's designated initializers):

```bash
clang++ -std=c++20 -O0 -Wno-c99-designator -Isrc \
  -DLOXPP_DEBUG_PRINT_CODE \
  src/*.cpp src/stdlib/*.cpp -o /tmp/loxpp-dis
```

(or `cmake -DLOXPP_DEBUG_PRINT_CODE=ON` for the same effect). Then:

```bash
/tmp/loxpp-dis test/translation-probes/01_assign_local.lox
```

Each probe prints the disassembly of every compiled chunk, followed by the
program's runtime output. Slot 0 of every chunk is the callee/receiver, so the
first *user* local is slot 1.

## Index

The `Problem(s)` column maps each probe to the problem IDs in
`../../docs/reference/bytecode-translation-problems.md`; see the verdict table below for whether
each has an off-the-shelf solution.

| Probe | Isolates | Problem(s) |
|---|---|---|
| `01_assign_local` | locals live on the operand stack; scope-exit `POP`; `SET_LOCAL` peeks | P1, P2 |
| `02_if_else` | `JUMP_IF_FALSE` peeks; branch `POP` discards the condition | P2, P3 |
| `03_and_or` | short-circuit value-carrying merge (needs `dup`) | P2, P3 |
| `04_while` / `05_for` | loop CFG from byte offsets; `for` desugaring has two back-edges | P3 |
| `06_shared_upvalue` | two closures capture one local → one shared cell | P4 |
| `08_call` | calling convention: stack args vs `Object[]`, slot 0 = callee | P5 |
| `09_class` | `this` = slot 0; `init` returns `this`; `SET_PROPERTY`/`DEFINE_METHOD` leave a value | P2, P5, P6 |
| `10_super` | `super` is compiled as an upvalue capture of the superclass | P4 |
| `11_for_in` | iterator + loop var are stack locals; `ITER_*` consume a loaded copy | P1, P8 |
| `12_list_map_index` | aggregate construction impedance; `SET_INDEX` leaves a value | P2, P7 |
| `13_enum_match` | `GET_TAG` (double) → `JUMP_TABLE` (int switch); default = `MATCH_ERROR` | P8 |
| `14_enum_payload` | enum payload read via `GET_INDEX`; `CALL` on an enum ctor | P6, P8 |
| `15_nested_arith` | operand-stack high-water mark (CIL `.maxstack`) | P1 |
| `16_slice_in` | `SLICE`/`IN` arg order and arity | P8 |
| `17_super_value` | `GET_SUPER` reads a super method as a value, not a call | P4 |
| `18_peek_of_named_local` | a `SET_LOCAL` peek reads a value the abstract-stack analysis already folded into a named local, not a stack temp | P1, P2 |
| `19_peek_of_named_local_global` | same fact through `SET_GLOBAL`, inside a block scope | P1, P2 |
| `20_float_imprecise_constant` | a number constant a 32-bit float cannot hold exactly | P2 |
| `21_exponent_constant` | a number constant whose exponent text has no decimal point (jasmin rejects it) | P2 |
| `22_and_or_assignment_statement` | an `and`/`or` right side ends in an assignment; the merge `POP` is also a CFG block leader | P2, P3 |
| `23_and_or_local_initializer` | `JUMP_IF_FALSE` on a condition the abstract-stack analysis already folded into a named local, not a stack temp | P2, P3 |
| `24_call_before_closure` | a global function called before its own `fun` declaration has run: late-bound globals must fail, not silently succeed | P5 |
| `25_seq_map_string_coverage` | `IS_SEQ` via a discarded `match` sequence pattern (list, string, map subject), `SLICE` on a List, `in` on a String and a Map, iteration over a String and a Map, `BUILD_MAP` with 3 pairs | P7, P8 |
| `26_enum_match_dispatch_and_error` | dense enum `match` dispatches to the correct arm; every arm's guard false raises a real, reachable `MATCH_ERROR` through the sparse compare-and-branch form | P8 |
| `27_jump_table_default_cross_enum` | the literal `JUMP_TABLE` "out of range" default, reached with a table-eligible match's subject from a DIFFERENT enum than its arms | P8 |
| `28_folded_match_operand_family` | `normalizeFoldedOperands`'s own required coverage: the nine folded-operand shapes (`BUILD_LIST`, `BUILD_MAP`, `CALL`, `GET_PROPERTY`, `INVOKE`, `SET_PROPERTY`, `SET_INDEX`, `SLICE`, `IN`, each with a folded operand) plus one nested match subject; `RETURN` of a folded match is covered separately by `105_return_folded_match` | P8 |
| `29_os_access` | the OS/world access natives (`args`, `env`, `exists`, `is_dir`, `is_file`, `stat`, `sleep`) must run byte-identically on every backend — the JVM runtime registers them too (`LoxRuntime`), and this probe guards against a future drift | none (parity gate) |
| `30_bool_compare_and_string_literal` | every comparison spelling (`==`, `!=`, `>`, `>=`, `<`, `<=`, each lowering to EQUAL/GREATER/LESS optionally paired with NOT), a standalone `!`, `%`, the `true`/`false`/`nil` literals printed as values in their own right, and a string constant with a quote, a backslash, and a tab | P2 |
| `31_deep_recursion` | native's own frame-count ceiling (`src/vm.h` `FRAMES_MAX`), reached through ordinary self-recursive `CALL`s; the JVM backend now mirrors the same ceiling (issue #268), so it must fail identically here too | P5, P6 |
| `53_deep_recursion_boundary` | the success side of the boundary `31_deep_recursion` pins the failure side of: 1022 nested calls is the deepest depth every backend still accepts, except the bootstrap interpreter, which may support a lower depth (spec/04-semantics.md, call depth exception) | P5, P6 |
| `57_stack_overflow_catchable` | unbounded recursion overflows `LoxClosure`'s own frame-count ceiling; a live `try`/`catch` must catch a real `Error` value with kind `StackOverflowError`, matching native (spec/04-semantics.md) — promoted into this directory (issue #322) | P6 |
| `58_stack_overflow_reentrant_fatal` | a second `StackOverflowError` raised by a deferred call while the first is still unwinding is fatal, not delivered to any `catchBlock` (spec/04-semantics.md line 1120) — promoted into this directory (issue #322), companion to `57_stack_overflow_catchable` | P5, P6 |
| `catch_overflow` | `LoxClosure`'s frame-count ceiling delivers a catchable `StackOverflowError`, matching native's kind, message, and post-catch continuation — promoted from jvm-only (issue #445) | P5, P6 |
| `defer_overflow_during_unwind` | a `StackOverflowError` raised by a deferred call while another one is still unwinding is fatal on every backend, not delivered to any `catchBlock` (`spec/04-semantics.md` line 1120) — promoted from jvm-only (issue #322) | P5, P6 |
| `jvm-only/defer_replaces_overflow` | a deferred call's own throw, uncaught within it, replaces a `StackOverflowError` still unwinding (`spec/04-semantics.md` defer Statement step 5); the unwind guard must still clear so a later, unrelated overflow is caught — JVM-only, see the note below the table | P5, P6 |
| `defer_overflow_kind_spoof` | a plain instance whose `kind` field spoofs `"StackOverflowError"`, thrown and caught entirely inside a deferred call, must not clear the unwind guard early while a real `StackOverflowError` is still unwinding — promoted from jvm-only (issue #322) | P5, P6 |
| `defer_throw_on_normal_return_during_unwind` | a deferred call's throw on a normal return (no fault propagating out of the deferring frame) must not be handed the unwind guard's identity, even while an unrelated `StackOverflowError` unwinds elsewhere — promoted from jvm-only (issue #322) | P5, P6 |
| `defer_replaces_unrelated_fault_during_unwind` | a deferred call's throw that replaces a different, unrelated fault must not be handed the unwind guard's identity while an unrelated `StackOverflowError` unwinds elsewhere — promoted from jvm-only (issue #322) | P5, P6 |
| `32_string_nul` | a string literal holding an embedded NUL byte (`\0`): `print` must write every byte, including the text after the NUL, on every backend, the bootstrap included (issue #129, issue #634); CI names it in the bootstrap differential step "NUL probes" and in the JVM and QBE bootstrap-wrapper gates (issue #654) | none (parity gate) |
| `33_class_pattern_match_error` | a `match` whose arms are all class patterns raises a real, reachable `MATCH_ERROR` when no arm matches, through the same fused opcode as the enum case | P8 |
| `34_match_consumed_result` | a `match` expression's result, once its own closing `POP` retires the synthetic subject local, is exposed as a named local's own value — `PRINT` and `DEFINE_GLOBAL` each need their own fold-aware read, the same way `RETURN` and `SET_GLOBAL` already do | P1, P2 |
| `37_invoke_field_bound_native_method` | a bound built-in method stored in an instance field and invoked through the field name drives `INVOKE`'s field-shadow arm, not `GET_PROPERTY` followed by `CALL` (spec/04-semantics.md) | P5, P6 |
| `38_bound_native_method_identity` | a Map or File method read through `GET_PROPERTY` (`m.has`) is a fresh per-read bound value — no two reads are the same object, the rule `spec/03-types.md` gives a user-class bound method | P5, P6 |
| `40_reflection` | the reflection introspection natives (`type`, `fields`, `methods`, `getField`, `setField`, `hasField`, `callMethod`) must run byte-identically on every backend — native, JVM, and QBE alike (`src/stdlib/reflect_api.cpp`, `LoxRuntime.registerReflection`); fields/methods use single-element classes so their unspecified iteration order cannot cause a false divergence | none (parity gate) |
| `41_reflect_getfield_non_instance` | `getField()` on a non-Instance receiver is a runtime error on every backend — an `error_probes` entry in `tools/check_jvm_probes.sh` | none (error-parity gate) |
| `42_reflect_setfield_non_instance` | `setField()` on a non-Instance receiver is a runtime error on every backend — an `error_probes` entry | none (error-parity gate) |
| `43_reflect_fields_non_instance` | `fields()` on a non-Instance argument is a runtime error on every backend — an `error_probes` entry | none (error-parity gate) |
| `44_reflect_hasfield_non_instance` | `hasField()` on a non-Instance receiver is a runtime error on every backend — an `error_probes` entry | none (error-parity gate) |
| `45_reflect_methods_non_instance` | `methods()` on a non-Class argument is a runtime error on every backend — an `error_probes` entry | none (error-parity gate) |
| `46_reflect_callmethod_non_instance` | `callMethod()` on a non-Instance receiver is a runtime error on every backend — an `error_probes` entry | none (error-parity gate) |
| `47_reflect_callmethod_closure_method` | `callMethod()` on a closure-backed user method runs on every backend (issue #496): `this` binds to the receiver, forwarded args arrive in order, and a throw inside the method is catchable — a positive-parity `probes` entry | none (parity gate) |
| `62_map_instance_keys` | user-object map keys (issue #469): a class defining both `__hash__` and `__eq__` keys a map, an equal-but-distinct key retrieves the same value, a non-Number `__hash__`, a non-Boolean `__eq__`, a missing-method key, and a key method that writes the map are each a catchable error, and a scalar key and an equal Instance key that share a bucket resolve the same way on every backend — a positive-parity `probes` entry | none (parity gate) |
| `64_net_process` | sockets and subprocess (issue #516): a loopback TCP echo (`listen`/`connect`/`accept`, `close_write`, `read`) and a captured child (`run`) must print byte-identically on every backend — native, JVM, and QBE alike; `tools/check_jvm_probes.sh` names it directly, and the QBE gate walks the directory | none (parity gate) |
| `65_invoke_callable_field` | `obj.field(args)` calls a field that holds a callable Instance (`__call__`), a class, or a bound method, like a plain call does (issue #560) — a positive-parity `probes` entry | none (parity gate) |
| `66_task_spawn_order` | `task.spawn` does not run its function; the spawner runs until it suspends, and new tasks run in spawn order; a Task is `==` only to a handle of the same task — a spec probe | none (spec probe) |
| `67_send_never_suspends` | `task.send` returns `nil` and never suspends the sender; `task.receive` is first in, first out and does not suspend while the mailbox holds a message; `task.pending` is the mailbox length — a spec probe | none (spec probe) |
| `68_mailbox_unbounded` | a mailbox has no bound: a thousand sends to one task never fail, and `task.pending` shows the queue grow — a spec probe | none (spec probe) |
| `69_task_yield` | a `yield` in a spawned task sends it to the back of the run queue and evaluates to `nil`; a `yield` in a coroutine the task resumed goes to its resumer; the main task follows the same rule, at the top level and in a function it calls — a spec probe | none (spec probe) |
| `70_sendable_set` | the sendable set of `spec/03-types.md` over every value kind a program can make: the messages are sent, every other kind raises `SendError`, on the same worker too, with the `SendError` message text — a spec probe | none (spec probe) |
| `71_message_identity` | a send copies nothing: a received Enum value and a returned Task handle are `==` to the originals, and a received String is `==` by its characters — a spec probe | none (spec probe) |
| `72_dead_task_same_worker` | a send to a task that has ended raises `DeadTaskError`, with its message text — a spec probe | none (spec probe) |
| `73_dead_task_dead_worker` | a send to a task whose worker has ended raises `DeadTaskError`; the probe sends until it raises, because the end and the send can race — a spec probe | none (spec probe) |
| `74_worker_isolation` | `task.spawn_worker` defines every top-level declaration on the new worker and runs no other statement, so a top-level `var` is undefined there; globals are per worker; an Enum from another worker matches its patterns; a field, the spawn message, and a value that `task.send` carries there and back each come back `==` — a spec probe | none (spec probe) |
| `75_spawn_worker_send_error` | `task.spawn_worker` with an argument that is not a message raises `SendError`, and no worker starts — a spec probe | none (spec probe) |
| `76_deadlock` | the main task waits in `task.receive()` with no task able to run on any worker: the program ends with `DeadlockError`, which no `try` catches and which runs no deferred call — a spec probe | none (spec probe) |
| `77_receive_across_native` | a `task.receive()` that would suspend inside a native callback raises `YieldAcrossNativeError`; one that finds a message works there — a spec probe | none (spec probe) |
| `78_receive_in_coroutine` | a receive inside a coroutine suspends the whole task; the coroutine stays `"running"`, so another task's resume of it is `RunningCoroutineError` — a spec probe | none (spec probe) |
| `79_uncaught_fault_in_task` | an uncaught throw in a spawned task ends the program after the task's own deferred calls run; the main task's `try` does not catch it, and a second task that would wake the main task never runs — a spec probe | none (spec probe) |
| `80_main_end_abandons` | the program ends when the main task ends: a task that has not run never runs, and a waiting task's deferred call does not run — a spec probe | none (spec probe) |
| `81_exit_from_task` | `exit(code)` in a spawned task ends the program at once — a spec probe | none (spec probe) |
| `82_send_order_across_workers` | messages from one worker to one task arrive in the order of the sends; a handle a worker sends of itself is `==` to the one `task.spawn_worker` returned — a spec probe | none (spec probe) |
| `83_blocking_call_blocks_one_worker` | a blocking call blocks only its own worker: one worker waits for ever in `Server.accept()`, and the main task on another worker keeps running and ends the program; an implementation that blocks every worker hangs and fails on the probe's `TIMEOUT` — a spec probe; it does not bind the bootstrap interpreter (its `NOT-BINDING` line, spec/README.md, Conformance), which `bootstrap-only/one_thread_blocking_call` checks in its place | none (spec probe) |
| `bootstrap-only/one_thread_blocking_call` | the bootstrap interpreter runs every worker on one host thread, so a blocking call on a worker also blocks the main task (spec/README.md, Conformance: scheduling is implementation-defined) — a spec probe that only the bootstrap gate runs | none (spec probe) |
| `84_spawn_worker_entry_closure` | `task.spawn_worker` with a function that is not from a top-level `fun` declaration (a nested function) is a fatal runtime error — a spec probe | none (spec probe) |
| `85_task_value` | a Task is truthy, has type `Task` and the form `<task>`, is not a valid map key (`InvalidMapKeyError`), and is not iterable (fatal) — a spec probe | none (spec probe) |
| `86_spawn_arity` | a spawned function runs with no arguments under the usual arity check; the uncaught `ArityError` in the task ends the program, so a second task that would wake the main task never runs — a spec probe | none (spec probe) |
| `87_yield_in_map_hash` | a `yield` in `__hash__` that a map write calls raises `YieldAcrossNativeError`, in the main task outside every coroutine too — a spec probe | none (spec probe) |
| `88_yield_in_map_eq` | a `yield` in `__eq__` that a map calls on a hash collision raises `YieldAcrossNativeError`, in the main task outside every coroutine too — a spec probe | none (spec probe) |
| `89_yield_in_defer` | a `yield` in a deferred call, and a `task.receive()` on an empty mailbox in a deferred call, raise `YieldAcrossNativeError`, in the main task outside every coroutine too — a spec probe | none (spec probe) |
| `90_credit_channel_bound` | the credit channel of `examples/credit_channel.lox` bounds the mailbox of a slow consumer: plain sends make `task.pending` reach the full count, and over a channel of capacity N the consumer's `task.pending` never exceeds N, which is within the bound of N plus one control slot per peer, on one worker and with the producer on a second worker — a spec probe | none (spec probe) |
| `91_credit_channel_deadlock` | a producer with no credit waits on its own `task.receive()`: the consumer never returns a credit, and the program ends with `DeadlockError` after the producer sends N items — a spec probe; `test/test_task.cpp` checks that the native report names both tasks | none (spec probe) |
| `92_deadlock_report` | the `DeadlockError` report carries the message of `spec/04-semantics.md`, Worker and Task Faults, on stderr; the main task and a spawned task both wait in `task.receive()` — a spec probe that pins the message text on every consumer | none (spec probe) |
| `93_task_spawn_function_boundmethod` | `task.spawn` accepts every Function and every BoundMethod — a native function (`clock`) and a bound native method (`m.keys`), not only a user closure (issue #608, spec/04-semantics.md, task.spawn step 1) — a spec probe | none (spec probe) |
| `94_coroutine_create_function_boundmethod` | `coroutine.create` accepts every Function and every BoundMethod — a native function (`clock`) and a bound native method (`m.keys`) each make a coroutine whose first resume returns the entry's result (issue #608, spec/05-stdlib.md, coroutine.create) — a spec probe | none (spec probe) |
| `95_cpu_bound_task_across_workers` | a task that computes without a suspend point stops only its own worker: 64 workers each run one spawned CPU-bound task, and the main task receives one message from every worker — a spec probe; an implementation with one carrier for each processor hangs, and the probe's `TIMEOUT` catches it; a machine with 64 or more processors does not show the defect, so the probe cannot fail there; it does not bind the bootstrap interpreter, whose `NOT-BINDING` line names the probe below | none (spec probe) |
| `96_cpu_bound_coroutine_across_workers` | the same rule for a coroutine: 64 workers each resume one CPU-bound coroutine, and the main task receives one message from every worker — a spec probe with the same `TIMEOUT`, the same 64-worker limit, and the same bootstrap exception | none (spec probe) |
| `97_class_pattern_instance` | a class pattern matches an instance of its class or of a subclass, and no other value, in each form: the bare name, the named-field form, an or-chain, and the `@`-binding; a bare class name in the arm body still names the class; the last `match` has no arm that matches and raises `MatchError` (issue #635) — a spec probe | none (spec probe) |
| `98_class_name_field_binding` | a field name in a class pattern (`Box{Item}`) is a binding, also when a global class has that name — a spec probe | none (spec probe) |
| `99_class_name_ctor_arg_binding` | a name in a positional constructor pattern (`W(Shape)`) is a binding, also when a global class has that name — a spec probe | none (spec probe) |
| `100_class_name_list_elem_binding` | a name in a sequence pattern (`[Shape, b]`) is a binding, also when a global class has that name — a spec probe | none (spec probe) |
| `101_class_declared_after_match` | a bare pattern name is a class test only when a global class of that name is declared earlier in the source; the test reads the global when it runs, so a name whose global no longer holds a class matches no value — a spec probe | none (spec probe) |
| `102_string_cr_escape` | the `\r` escape in a string literal is one carriage return (U+000D), and the characters around it stay in place (`spec/01-lexical.md`, string escapes); read with `len` and `ord`, so no CHECK line holds a carriage return (issue #651) — a spec probe | none (spec probe) |
| `103_undefined_global` | a global is undefined until its declaration runs: a read, an assignment, and a call that reads it each raise a catchable `UndefinedVariableError`, and the same names work after the declaration; it covers the QBE backend's unbound global slot (issue #592) — a spec probe | none (spec probe) |
| `104_for_in_not_iterable_fatal` | a for-in over a value that is not a List, String, Map, or Coroutine is a halting fault: the fault occurs, the program prints nothing and exits non-zero (`spec/04-semantics.md`, Halting Faults; issue #617) — a spec probe | none (spec probe) |
| `105_return_folded_match` | a function returns a `match` expression: the result is a named local, so `RETURN` sees operand depth 0. Both the sparse literal-arm form and the enum `JUMP_TABLE` form must return the right value on every backend (issue #647) — a spec probe | P1, P8 |
| `106_enum_name_field_binding` | a field name in a class pattern (`Box{Red}`) is a binding, also when an enum constructor has that name (issue #648) — a spec probe | none (spec probe) |
| `107_enum_declared_after_match` | a bare pattern name is a constructor tag test only when an enum constructor of that name is declared earlier in the source; in a function declared before the enum, the name is a binding (issue #648) — a spec probe | none (spec probe) |
| `108_enum_bare_name_tag_test` | a bare constructor name is a tag test also for a constructor with fields, alone, in an `or`-chain and as the sub-pattern of an `@`-binding (issue #648) — a spec probe | none (spec probe) |
| `109_enum_ctor_over_class` | when an enum constructor and a global class have the same name, a bare pattern name is a tag test (issue #648) — a spec probe | none (spec probe) |
| `110_index_assign_eval_order` | an index assignment evaluates the collection, the index and the value before it checks the target or the index: a collection expression's effect prints before the index's, a List, an invalid map key and a non-indexable target print `VALUE` before their catchable fault, a List check sees a List the value grew, and a String prints `INDEX` and `VALUE` before its halting fault (`spec/04-semantics.md`, Index Set; `spec/README.md`, Conformance; issue #751) — a spec probe | none (spec probe) |
| `111_task_spawn_native_entry_receive_raises` | a task whose entry is a native function cannot suspend in `task.receive`: the native has no frame to rebuild, so the receive raises `YieldAcrossNativeError` (`spec/04-semantics.md`, task.receive step 3, issue #723) — a spec probe | none (spec probe) |
| `112_coroutine_native_entry_receive_raises` | a coroutine whose entry is a native raises `YieldAcrossNativeError` on a receive with an empty mailbox, the resumer catches it, and the coroutine becomes dead (`spec/04-semantics.md`, co.resume step 5, issue #723) — a spec probe | none (spec probe) |
| `113_coroutine_native_resume_entry_receive_raises` | a native entry that resumes another coroutine keeps the host frame on the chain: the inner receive raises, and both coroutines are dead (issue #723) — a spec probe | none (spec probe) |
| `114_task_spawn_native_resume_entry_receive_raises` | the same native resume-entry chain inside a spawned task: the task has no handler, so the receive fault ends the program (issue #723) — a spec probe | none (spec probe) |
| `115_task_spawn_native_entry_receive_with_message` | a receive below a native entry that finds a message returns at once, so the task ends; a second send then raises `DeadTaskError`, which proves the task consumed the message and is not parked (issue #723) — a spec probe | none (spec probe) |
| `116_coroutine_native_entry_receive_with_message` | a coroutine whose entry is a native returns that native's result, and the coroutine is dead; the message is already present, so no suspension happens (issue #723) — a spec probe | none (spec probe) |
| `117_coroutine_native_entry_for_in` | a for-in over a coroutine whose native entry returns without yielding runs its body zero times and leaves the coroutine dead (issue #723) — a spec probe | none (spec probe) |
| `118_list_pop_read_faults` | a List built-in method is not a value: a read of `l.pop` without a call is a halting fault, while `l.append(x)`, `l.pop()` and `l.remove(x)` still run (`spec/04-semantics.md`, Property Get step 6 and Halting Faults; issue #707) — a spec probe | none (spec probe) |
| `119_defer_list_method_callee_faults` | the callee of `defer` is evaluated as steps 1-2 of Function Call, so `defer l.append(1)` faults on the property read at the defer statement (`spec/04-semantics.md`, defer Statement and Property Get step 6; issue #674) — a spec probe | none (spec probe) |
| `121_class_pattern_missing_field` | a name in the braces of a class pattern is a property read of the matched instance: a nil-valued field binds nil, a method name binds a bound method, and a name that is neither is a halting fault (`spec/04-semantics.md`, match Expression, "Class patterns", and Property Get; issue #748) — a spec probe | none (spec probe) |
| `122_destructure_property_reads` | a destructuring declaration reads each name as a property read: a Map gives a bound method, an Error gives `kind` or the catchable `UndefinedPropertyError` (`spec/04-semantics.md`, Destructuring Declaration; issue #748) — a spec probe | none (spec probe) |
| `123_destructure_missing_field` | a destructuring declaration whose name is neither a field nor a method of an instance is a halting fault (`spec/04-semantics.md`, Destructuring Declaration; issue #748) — a spec probe | none (spec probe) |
| `124_destructure_non_instance_faults` | a destructuring declaration whose source is a Number is a halting fault, also inside a `try`, because a Number is not in Property Get steps 2-5 (`spec/04-semantics.md`, Destructuring Declaration and Property Get step 6; issue #748) — a spec probe | none (spec probe) |
| `125_map_destructure_missing_name` | a Map is in Property Get steps 2-5, so a destructured name that is not a Map method is the Map's own halting fault (`spec/04-semantics.md`, Destructuring Declaration and Property Get step 2; issue #748) — a spec probe | none (spec probe) |
| `126_math_property_missing_name` | the math object has fixed fields, so a name that is not a math function or constant is a halting property fault, also through destructuring (`spec/04-semantics.md`, Property Get steps 7-9; issue #748) — a spec probe | none (spec probe) |
| `127_map_literal_eval_order` | a map literal evaluates every key-value pair in source order before it validates any key, and validates every key before it inserts any pair: a later pair still runs after an earlier invalid key, a valid Instance key's `__hash__` does not run when a later key is invalid, and a duplicate key keeps the last value (`spec/04-semantics.md`, Map Literal, steps 1-3; issue #754) — a spec probe | none (spec probe) |
| `128_underscore_structural_pattern` | a `_` followed by `(` or `{` is a structural pattern head, not the wildcard, so it can name a constructor with fields (`spec/02-syntax.md`, armPat) — a spec probe | none (spec probe) |
| `129_enum_shared_constructor_name` | two enums may share a constructor name, and a constructor pattern matches only values of its own enum, also at the same position (issue #691, issue #696) (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `130_match_or_literal_alternatives` | `or` joins literal alternatives, and a literal and a constructor pattern in one arm (issue #756) (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `131_enum_bare_constructor_name` | a constructor name alone is an ordinary variable in an expression and a binding in a pattern (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `132_enum_name_alone_rejected` | the enum name with no `.` and constructor after it is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `133_enum_unknown_constructor_rejected` | `E.B` with no constructor `B` in `E` is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `134_enum_name_as_local_rejected` | a local variable with the name of an enum is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `135_enum_name_global_collision_rejected` | a global variable with the name of an enum is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `136_enum_use_before_declaration_rejected` | `E.A` before the declaration of `E` in the source is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `137_enum_name_assignment_rejected` | an assignment to an enum name is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `138_enum_constructor_assignment_rejected` | an assignment to `E.A` is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `139_ctor_pattern_field_count_rejected` | a positional constructor pattern with fewer names than fields is a compile error (issue #747) (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `140_ctor_pattern_unknown_constructor_rejected` | a constructor pattern `E.B` with no constructor `B` in `E` is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `141_match_comma_chain_rejected` | a comma between literal patterns is a parse error (issue #756) (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `142_bare_positional_pattern_rejected` | a positional pattern with no enum name, `A(x)`, is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `143_exhaustiveness_per_enum_rejected` | a match with constructor patterns of two enums checks exhaustiveness for each enum, also when the first enum is complete (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `144_ctor_pattern_too_many_fields_rejected` | a positional constructor pattern with more names than fields is a compile error (issue #747) (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `145_ctor_pattern_non_enum_no_match` | a constructor pattern does not match a value that is not an enum value, and the match goes on to the next alternative or arm (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `146_enum_hides_builtin_global` | an enum name may be the name of a built-in global, and the enum hides the built-in in the whole program (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `147_enum_name_as_parameter_rejected` | the enum name used for a parameter, in a function before the enum declaration is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `148_enum_name_as_pattern_binding_rejected` | the enum name used for a pattern binding is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `149_enum_name_as_catch_variable_rejected` | the enum name used for a `catch` variable is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `150_enum_name_as_for_in_variable_rejected` | the enum name used for a `for`-in variable is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `151_enum_name_as_local_function_rejected` | the enum name used for a local function is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `152_enum_name_in_destructuring_rejected` | the enum name used for a name in a destructuring declaration is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `153_enum_name_as_at_binding_rejected` | the enum name used for an `@`-binding name is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `154_enum_name_as_local_class_rejected` | the enum name used for a local class is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `155_enum_qualified_constructor` | a program writes a constructor as `E.A` in expressions and in positional, named, tag-only and `@` patterns; a constructor is a value, and `str` names the enum and the constructor (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `156_builtin_before_enum_declaration_rejected` | a call of a built-in global before the declaration of an enum with its name is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `157_enum_name_before_declaration_rejected` | a bare enum name before its declaration is a compile error (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `158_enum_name_as_member_name` | a constructor, a constructor field, a method and a property may have the name of an enum (`docs/decisions/enum-constructor-namespaces.md`, mission #771) — a spec probe | none (spec probe) |
| `159_enum_bare_and_qualified_constructor_identity` | the bare constructor name and `E.A` are the same constructor object; the JVM backend once made a second object for each load of the constructor constant. N6 (#777) deletes this probe with the bare form | JVM |
| `bootstrap-only/cpu_bound_work_blocks_all_workers` | the bootstrap interpreter runs every worker on one host thread, so a task that computes without a suspend point also stops every task of every other worker (spec/README.md, Conformance: scheduling is implementation-defined); probes 95 and 96 do not bind the bootstrap, and this probe checks the bootstrap's own choice in their place — a spec probe that only the bootstrap gate runs | none (spec probe) |
| `bootstrap-only/stdout_no_trailing_newline` | the last bytes that a program writes to stdout have no newline after them: the bootstrap wrapper must pass them on (issue #634), also when the interpreter runs on the JVM backend (issue #654); `check_spec_probes.py` and the CI NUL-probe steps run it, and the JVM and QBE bootstrap-wrapper gates run it too. It stays in `bootstrap-only/` because it pins the wrapper's stdout filter, not backend semantics | none (parity gate) |
| `bootstrap-only/stdout_carriage_return` | a program prints a carriage return (byte 13) and the bootstrap wrapper passes it on (issue #656); `tools/diff_runtimes.py` and `check_spec_probes.py` read stdout in text mode, which turns each CR into a newline, so `tools/check_cr_bytes.py` compares the raw stdout bytes with native; CI runs it in the bootstrap step "NUL probes". It has no STATUS line, because a CHECK line cannot hold a CR byte | none (parity gate) |
| `bootstrap-only/stdout_carriage_return_before_newline` | a program prints a carriage return (byte 13) directly before a newline (issue #701): text mode turns CR LF into one newline, so a wrapper that drops that CR passes the text-mode gates and only `tools/check_cr_bytes.py`, which compares the raw stdout bytes with native, catches it; CI runs it in the bootstrap step "NUL probes". It has no STATUS line, because a CHECK line cannot hold a CR byte | none (parity gate) |
| `48_file_visible_after_close` | a File's write-close-reopen cycle: once `close()` returns, a fresh `open()` on the same path sees everything the closed handle wrote (`spec/05-stdlib.md`, File section) — pins the after-close half of the visibility guarantee, not the buffered-before-close half, which is implementation-defined and allowed to differ | none (parity gate) |
| `55_for_in_map_net_zero` | an erase plus an insert in one `for`-in body restores the net size, so a size check misses it — the structural version check must still fail on every backend, with empty stdout (issue #362) — an `error_probes` entry | none (error-parity gate) |
| `jvm-only/61_operator_overload` | operator overloading (issue #472): every T1 dunder method (`__add__ __sub__ __mul__ __div__ __mod__ __neg__ __lt__ __gt__ __eq__ __contains__ __call__`), the Wave-2 methods (`__index_get__ __index_set__ __len__ __iter__`), and the deferred `__slice__` dispatch on an Instance; `!=`/`<=`/`>=` derive from `__eq__`/`__gt__`/`__lt__`, and the built-in List/String/Map/Number paths stay unchanged. Also the reflected methods (issue #508): a built-in left operand defers to the right operand's `__radd__ __rsub__ __rmul__ __rdiv__ __rmod__ __rlt__ __rgt__` or `__eq__`, with `<=`/`>=` deriving from the reflected `__rgt__`/`__rlt__` — named directly by both check scripts, see the note below the table | P6 |
| `jvm-only/known-divergence/52_fat_frame_stack_divergence` | native's value-stack ceiling only: a frame with 20 locals, recursed 900 deep, overflows `src/vm.h` `STACK_MAX` (16384 slots) while the call chain stays under `FRAMES_MAX` (1024); native fails, but the JVM backend mirrors only `FRAMES_MAX` (`runtime/jvm/src/lox/LoxClosure.java`) and runs it to completion — a known divergence, named directly by `tools/check_jvm_probes.sh` (issue #492), see the note below the table | P5, P6 |
| `V1_fresh_cell` | body-local captured in a loop → **fresh cell/iter** → prints `0 1 2` | P4 |
| `V2_shared` | mutable shared upvalue → prints `2` | P4 |
| `V3_loopvar` | loop var captured directly → **one shared cell** → prints `3 3 3` | P4 |
| `V4_mutate_through_upvalue` | `set` writes a shared cell, `get` reads it back → prints `7 9` | P4 |
| `V5_self_recursive_closure` | a local `fun` captures its own slot (direct recursion) → prints `120` | P4 |
| `V6_self_recursive_closure_in_loop` | self-recursive local `fun`, fresh cell per loop trip → prints `12` | P4 |
| `48_try_catch_instance_undefined_property_fatal` | an undefined property read on an ordinary Instance is fatal, not caught, even with a live `try` (issue #253) | none (error-parity gate) |
| `49_try_catch_map_undefined_property_fatal` | an undefined property read on a Map is fatal even with a live `try` (issue #253) | none (error-parity gate) |
| `50_try_catch_non_instance_get_property_fatal` | `GET_PROPERTY` on a non-Instance receiver is fatal even with a live `try` (issue #253) | none (error-parity gate) |
| `51_try_catch_non_instance_set_property_fatal` | `SET_PROPERTY` on a non-Instance receiver is fatal even with a live `try` (issue #253) | none (error-parity gate) |
| `56_file_nul` | `File.read`/`readline`/`readlines` return the full binary content when it holds NUL bytes (issue #401); CI names it in the same bootstrap step "NUL probes" (issue #634) and in the JVM and QBE bootstrap-wrapper gates (issue #654) | none (parity gate) |
| `56_file_read` | `File.read()` with arity 0 exists on every consumer, the bootstrap included (issue #378) | none (parity gate) |
| `56_math_constants` | `math.inf` and `math.nan` are Numbers with the native values, not a native-fn value (issue #329) | none (parity gate) |
| `57_defer_arity_fatal_fast_path` | the wrong-argument-count fatal fast path skips every pending `defer` when no `try` is live — empty stdout, no `defer-g` (issue #319); CI names it in the bootstrap step "fatal fast path probes" (issue #636) | none (error-parity gate) |
| `58_defer_overflow_fatal_fast_path` | the call-stack-overflow fatal fast path skips every pending `defer` when no `try` is live — empty stdout (issue #319); CI names it in the bootstrap step "fatal fast path probes" (issue #636) | none (error-parity gate) |
| `59_return_out_of_try_leak` | a `return` out of a still-open `try` must not leak the JVM handler-liveness counter, or a later unrelated arity fault looks catchable (issue #319); CI names it in the bootstrap step "fatal fast path probes" (issue #636) | none (error-parity gate) |
| `60_throw_ends_try_binding` | a `throw` unwinds past try-body locals without `endScope`, so the unwinding side ends their bindings itself (issue #386) | P1, P4 |
| `63_ord_chr_string_order` | `ord()`/`chr()` byte conversion and String byte ordering on every backend (issue #507) | none (parity gate) |
| `defer_runs_before_caught_throw_propagates` | a deferred call runs when its own function exits by `throw`, before the fault propagates to the caller's `catch` (`spec/04-semantics.md`) | none (parity gate) |
| `defer_runs_on_normal_return` | a deferred call runs at the right point on an ordinary `return`, and its return value does not change the already-computed result | none (parity gate) |
| `defer_super_method` | `defer super.m(...)` defers a call to the superclass method; the arguments evaluate at the `defer`, the call at function exit (`spec/02-syntax.md`, `deferStmt`) | none (parity gate) |
| `jvm-only/fault_defer_class_construction_fatal` | a `defer`red class construction is fatal, not delivered to any `catchBlock` | none (error-parity gate) |
| `jvm-only/fault_defer_noncallable_value_fatal` | a `defer`red non-callable value is fatal, not delivered to any `catchBlock` | none (error-parity gate) |
| `jvm-only/fault_enum_index_fractional_fatal` | an enum payload read with a fractional index is fatal, not a catchable Error | none (error-parity gate) |
| `jvm-only/fault_enum_index_range_fatal` | an enum payload read out of range is fatal, through `RAISE_ERROR`, not a catchable Error | none (error-parity gate) |
| `jvm-only/fault_enum_index_type_fatal` | an enum payload read with a non-Number index is fatal | none (error-parity gate) |
| `jvm-only/fault_invoke_field_not_callable_fatal` | `INVOKE` on a non-callable field is fatal | none (error-parity gate) |
| `jvm-only/fault_invoke_method_not_found_fatal` | `INVOKE` of a missing method is fatal | none (error-parity gate) |
| `jvm-only/fault_reflect_callmethod_on_error_fatal` | `callMethod` on a caught Error (a `LoxInstance` under the hood) is fatal | none (error-parity gate) |
| `jvm-only/fault_reflect_fields_on_error_fatal` | `fields` on a caught Error is fatal | none (error-parity gate) |
| `jvm-only/fault_reflect_getfield_on_error_fatal` | `getField` on a caught Error is fatal | none (error-parity gate) |
| `jvm-only/fault_reflect_hasfield_on_error_fatal` | `hasField` on a caught Error is fatal | none (error-parity gate) |
| `jvm-only/fault_reflect_setfield_on_error_fatal` | `setField` on a caught Error is fatal | none (error-parity gate) |
| `jvm-only/fault_set_index_string_fatal` | `SET_INDEX` on a String is fatal | none (error-parity gate) |
| `jvm-only/fault_set_property_on_error_fatal` | `SET_PROPERTY` on a caught Error is fatal | none (error-parity gate) |
| `jvm-only/fault_slice_non_sliceable_fatal` | a slice on an Instance with no `__slice__` is fatal, a raw `LoxError`, not a catchable Error | none (error-parity gate) |
| `jvm-only/fault_stringify_too_deep_in_try` | the stringify depth guard is fatal even inside a live `try` (the Halting Faults section) | none (error-parity gate) |
| `jvm-only/fault_stringify_too_deep_print` | a self-referential list printed 300 deep trips the stringify depth guard at `print` | none (error-parity gate) |
| `jvm-only/try_catch_enum_ctor_arity_message` | an enum constructor called with the wrong arity is catchable; `kind` and `message` match native | none (parity gate) |
| `jvm-only/try_catch_error_property_read_still_works` | a caught Error's `kind` and `message` fields stay readable through `GET_PROPERTY` | none (parity gate) |
| `jvm-only/try_catch_invoke_on_error_invalid_receiver` | `INVOKE` on a caught Error with an invalid receiver is itself catchable, with the matching `kind`/`message` | none (parity gate) |
| `jvm-only/try_catch_set_index_not_indexable` | `SET_INDEX` on a non-indexable value is catchable, with the matching `kind`/`message` | none (parity gate) |
| `jvm-only/try_catch_undefined_variable_set_message` | assigning to an undefined variable is catchable, with the matching `kind`/`message` | none (parity gate) |
| `qbe-only/deep_recursion` | every compiled Lox call is a real C call (`rt_call` → `Runtime::call` → the callee's own `call`), so QBE's C-stack depth must match native's frame-count ceiling for plain recursion | P5, P6 |
| `qbe-only/deep_recursion_wide` | the per-function `STACK_MAX` check (`emitStackCheck`), not the pre-existing `FRAMES_MAX` guard: a wide frame overflows the value stack before the call chain is deep | P5, P6 |
| `qbe-only/fib` | naive recursive Fibonacci: the `CALL`/`RETURN` round-trip with no closures | P5 |
| `qbe-only/s8_promotion_across_call` | a promoted local reassigned a fresh list each iteration must spill to its slot before an allocating call, or `LOXPP_STRESS_GC=1` frees it (issue #461) | none (QBE-only gate) |
| `qbe-only/s8_promotion_captured` | a local a nested closure captures stays in its stack slot, never promoted, and the closure observes later writes across a GC | none (QBE-only gate) |
| `qbe-only/s8_promotion_loop` | a non-captured loop counter and accumulator carried across the back-edge by a phi give the same result under `LOXPP_STRESS_GC=1` | none (QBE-only gate) |
| `qbe-only/V1_fresh_cell_no_lists` | the `V1_fresh_cell` hazard with no list opcodes (S4): a captured loop-body local needs a fresh cell per iteration, checked through globals instead of a list slot | P4 |
| `qbe-only/V3_loopvar_no_lists` | the `V3_loopvar` hazard with no list opcodes (S4): a closure capturing the loop variable itself shares one open upvalue across every iteration | P4 |

The `66` to `96` probes are **spec probes** for the workers and tasks of
`spec/04-semantics.md`. A spec probe carries a `// STATUS: ok` or
`// STATUS: error` line and one `// CHECK:` line per line of the stdout the
spec defines for it. A probe that ends through `exit(code)` also carries
`// EXIT: <code>`, and a probe that fails by hanging carries
`// TIMEOUT: <seconds>`. A probe never pins stderr text: every report and
message is implementation-defined (`spec/README.md`, Conformance), and
`tools/check_spec_probes.py` rejects a `// STDERR:` line. For a
`STATUS: ok` probe the whole stdout must equal the CHECK lines. For a
`STATUS: error` probe the exit status must be non-zero and the stdout must
start with the CHECK lines: the output before a halt binds, and the deferred
calls of a halt may print more. A probe whose fault is a halting fault puts
no `try` around it, because an implementation may make a halting fault
catchable. `tools/check_spec_probes.py` runs every spec probe of this
directory on one consumer. CI runs it on the native VM, the bootstrap interpreter, the JVM
backend, and the QBE backend. `tools/diff_runtimes.py` compares the JVM, QBE,
and bootstrap runs with native; the bootstrap walk takes only the spec
probes (`--only-spec-probes`), and separate CI steps name `32_string_nul`,
`56_file_nul`, and `bootstrap-only/stdout_no_trailing_newline` for the JVM and
QBE bootstrap wrappers (issue #654). A consumer that does not pass a probe yet
can list it in an expected-failure file, which both tools read with
`--xfail`: a listed probe must fail, and a listed probe that passes fails
the gate until its line is deleted.

One rule of the Workers and Tasks section has no spec probe: a `print`
line from one worker never mixes with a line from another worker. Its
output order depends on timing between threads, and a spec probe compares
an exact stdout. `tools/check_print_lines.py` checks it with
`test/print-lines/two_workers.lox`, and does not use the order of lines
between workers.

`jvm-only/61_operator_overload` and `jvm-only/defer_replaces_overflow` sit one
level down because this directory's own files are walked non-recursively by
`tools/diff_runtimes.py` (native against the JVM backend). The
operator-overloading probe is named directly by `tools/check_jvm_probes.sh`
and `tools/check_qbe_probes.sh`; `defer_replaces_overflow` is named by
`tools/check_jvm_probes.sh` for the JVM checkpoint.

`jvm-only/known-divergence/52_fat_frame_stack_divergence` sits a second level
down for the opposite reason: it records a divergence on purpose (native must
fail, the JVM backend must succeed), so it must stay off every generic
byte-for-byte walk. `tools/diff_runtimes.py` walks a directory non-recursively,
so the CI step over `test/translation-probes/jvm-only/` never reaches it;
`tools/check_jvm_probes.sh` names it directly instead.

`53_stack_overflow_catchable` and `54_stack_overflow_reentrant_fatal` have
been promoted into this directory as `57_stack_overflow_catchable` and
`58_stack_overflow_reentrant_fatal` (issue #322) now that the JVM backend has
catchable stack overflow parity. `31_deep_recursion` used to sit beside them
for the same reason; it moved directly into this directory once the JVM
backend grew its own frame-count ceiling to match (issue #268).

`defer_overflow_during_unwind`, `defer_overflow_kind_spoof`,
`defer_throw_on_normal_return_during_unwind`, and
`defer_replaces_unrelated_fault_during_unwind` used to sit in `jvm-only/`
too. They have been promoted into this directory (issue #322) now that the
JVM backend matches on these stack-overflow unwind guard behaviors.

## Are these problems solved in the literature?

Each problem was mapped against compiler literature and real dynamic-language
JVM backends. **None is research-open** — every one decomposes into named,
textbook techniques with abundant precedent. The difficulty concentrates in one
place: no mainstream dynamic-language backend lowers from a clox-style *fused*
stack bytecode (they all keep locals and temporaries distinct in their own IR),
which is exactly why **P1/P3/P4 are `SOLVED-WITH-ADAPTATION`** while
**P5/P7/P8 are plain `SOLVED`** that merely consume P1's output.

- **P4 is the sharpest hazard.** The technique is standard, but the current
  backend plans are *provably wrong* (`CLOSE_UPVALUE`-as-no-op with hoisted
  cells) in a **test-evading** way — `V3` passes (`3 3 3`) while `V1` is silently
  miscompiled (`2 2 2` instead of `0 1 2`).
- **P1 is the foundational systemic risk.** Everything downstream consumes its
  slot-vs-temporary classification; get it wrong and the output fails to
  *verify*, not merely misbehave.
- **P6 is not really a codegen problem** — it is a runtime-library contract to
  specify (one `Callable` interface, uniform boxing, dispatch helpers).

| Problem | Verdict | Technique — precedent |
|---|---|---|
| **P1** Abstract-stack reconstruction | `SOLVED-WITH-ADAPTATION` | Abstract interpretation of stack effects (the verifier's own data-flow) + stack→register de-stacking — *no tool ingests clox's fused local/stack bytecode*, so the local/temporary partition must be rebuilt first (Soot Baf→Jimple; ASM `COMPUTE_MAXS`) |
| **P2** Peek-not-pop stack effects | `SOLVED` | Stack-effect normalization: `dup`/`dup_x1` for assignment-as-expression, or fuse the `SET_*;POP` idiom into a plain store — *exactly what `javac` emits for value-producing stores* |
| **P3** CFG recovery + verifier legality | `SOLVED-WITH-ADAPTATION` | Leaders-algorithm CFG recovery + uniform `Object` at merges to satisfy the type-merge rule — must then synthesize JVM `StackMapTable` / CIL `.maxstack` (ASM `COMPUTE_FRAMES`; Jython/Clojure/Kotlin all do this) |
| **P4** Closures: capture + cell lifetime | `SOLVED-WITH-ADAPTATION` | Closure conversion + **assignment conversion** (box captured mutable vars into heap cells) — `CLOSE_UPVALUE` = *fresh cell per iteration*, not a no-op (cf. C# 5 loop-var change; Scala/Kotlin `Ref`; ORBIT/Dybvig) |
| **P5** Calling-convention impedance | `SOLVED` | ABI/prologue lowering + argument marshalling into a generic `call(receiver, Object[])`, with `RETURN` selecting `areturn`/void — *precisely Rhino `Callable.call`, Jython `__call__`, JRuby `DynamicMethod.call`* (consumes P1) |
| **P6** Runtime polymorphism behind opcodes | `RUNTIME-DESIGN` | Uniform boxed values + one dispatch interface/helper set — a runtime contract, not a codegen algorithm (cf. Clojure `IFn`/`RT`, Jython `PyObject`, DLR `CallSite`, Nashorn `invokedynamic`) |
| **P7** Aggregate construction from N temps | `SOLVED` | Spill the N stack temps then array-init (`dup`/`aastore` / `newarr`/`stelem`) — a mechanical reshape (AST compilers sidestep it entirely; cf. Clojure `VectorExpr`, Groovy) |
| **P8** Opcode fusion + switch/iterator lowering | `SOLVED` | `GET_TAG`→`tableswitch`/CIL `switch` with box/unbox peephole fusion; for-in → `hasNext()`/`next()` desugar — *cf. `javac`/Kotlin enum-switch (`$SwitchMap` + `tableswitch`)* (consumes P1/P3) |

### Key references

- **Verification / stack-state merge:** JVMS §4.10 (type-checking verifier, merge
  rule), §4.7.4 (`StackMapTable`); ECMA-335 (CLI) III §1.7.5 & §1.8.1.3, II §25.4.3
  (`.maxstack`); Leroy, *Java Bytecode Verification* (2003).
- **CFG / de-stacking:** Dragon 2e §8.4 (leaders algorithm), §8.7 (peephole);
  Vallée-Rai et al., *Soot* — Baf→Jimple (CASCON 1999); ASM `COMPUTE_MAXS`/`COMPUTE_FRAMES`.
- **Closures:** Appel, *Modern Compiler Implementation* Ch. 15 (closure conversion);
  ORBIT (SIGPLAN '86) & Dybvig (1987) (assignment conversion / boxing);
  Nystrom, *Crafting Interpreters* Ch. 25 (`closeUpvalues` at scope exit).
- **Calling convention / switches:** Appel Ch. 6 (activation records); JVMS §3.10
  (switches), §6.5 (`invoke*`, `dup` family, `tableswitch`); JLS §14.14.2 (for-each
  desugaring).
- **Precedents (dynamic language → JVM/.NET):** Jython, Rhino/Nashorn, JRuby,
  Clojure, Groovy, Kotlin, Scala; IronPython/IronRuby (DLR); GraalVM/Truffle.

> Verdicts are a literature-mapping pass over P1–P8; the full problem statements,
> evidence, and the implied backend pipeline are in
> `../../docs/reference/bytecode-translation-problems.md`.
