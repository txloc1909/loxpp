; Hand-written probe for the assemble -> run -> link chain, before any code
; generator exists. Calls lox.LoxOps.print directly, so it proves the same
; runtime entry point the JVM backend's generated PRINT opcode will call.
.class public HelloRt
.super java/lang/Object

.method public <init>()V
    .limit stack 1
    .limit locals 1
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static main([Ljava/lang/String;)V
    .limit stack 1
    .limit locals 1
    ldc "lox-rt ok"
    invokestatic lox/LoxOps/print(Ljava/lang/Object;)V
    return
.end method
