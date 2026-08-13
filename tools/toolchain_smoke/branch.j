; Branching bytecode — a forward conditional and a backward goto, the shapes the
; JVM backend emits for JUMP_IF_FALSE and LOOP.
;
; This is the fixture that matters for the frames question. Under class file
; version 50+ a method with these branches needs a StackMapTable, and the backend
; would have to compute one. Jasmin emits 45.3, so the old type-inferencing
; verifier applies and no table is required. If a future assembler swap starts
; emitting 50+, this stops verifying and the smoke check catches it.
.class public BranchProbe
.super java/lang/Object

.method public <init>()V
    .limit stack 1
    .limit locals 1
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static main([Ljava/lang/String;)V
    .limit stack 3
    .limit locals 2
    iconst_0
    istore_1
loop:
    iload_1
    iconst_3
    if_icmpge done
    iload_1
    iconst_1
    iadd
    istore_1
    goto loop
done:
    getstatic java/lang/System/out Ljava/io/PrintStream;
    ldc "branching ok"
    invokevirtual java/io/PrintStream/println(Ljava/lang/String;)V
    return
.end method
