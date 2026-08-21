// Deliberately malformed IL: a stack-depth mismatch across a control-flow
// merge, the exact defect class an IL verifier must catch and the one this
// node's IL-legality check depends on CoreCLR rejecting at load time (no
// ILVerify in this image; see tools/check_managed_toolchains.sh).
//
// The branch condition is an opaque call (IsNullOrEmpty on a non-constant
// string), not a literal the JIT can prove false. A literal condition lets
// the JIT fold the branch and delete the offending edge before any depth
// check runs, which would make this fixture pass for the wrong reason and
// stop testing what it claims to test.
//
// One path falls through past the branch and leaves a value on the stack;
// the branch path arrives at L1 with an empty stack. `pop` then either
// underflows the empty-stack path or leaves the other path's value stranded
// at `ret` — a merge disagreement ilasm does not check and CoreCLR must.
.assembly extern System.Runtime { .ver 8:0:0:0 }
.assembly StackMismatch {}
.module StackMismatch.dll

.class public auto ansi StackMismatch extends [System.Runtime]System.Object
{
  .method public static void Main() cil managed
  {
    .entrypoint
    .maxstack 2
    ldstr "x"
    call bool [System.Runtime]System.String::IsNullOrEmpty(string)
    brtrue L1
    ldc.i4.1
  L1:
    pop
    ret
  }
}
