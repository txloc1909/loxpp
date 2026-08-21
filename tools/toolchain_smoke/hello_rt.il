// Hand-written probe for the assemble -> run -> link chain, before any code
// generator exists. Calls Lox.LoxOps::Print directly, so it proves the same
// runtime entry point the CLR backend's generated PRINT opcode will call.
// tools/toolchain_smoke/HelloRt.j is the JVM equivalent; this mirrors it.
.assembly extern System.Runtime { .ver 8:0:0:0 }
.assembly extern System.Console { .ver 8:0:0:0 }
.assembly extern LoxRuntime {}
.assembly HelloRt {}
.module HelloRt.dll

.class public auto ansi HelloRt extends [System.Runtime]System.Object
{
  .method public static void Main() cil managed
  {
    .entrypoint
    .maxstack 1
    ldstr "lox-rt ok"
    call void [LoxRuntime]Lox.LoxOps::Print(object)
    ret
  }
}
