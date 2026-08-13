; Minimal Jasmin input: exercises the same .j -> .class -> java path the JVM
; backend will use, including Jasmin's automatic stack map frame computation
; (see notes/jvm-backend-plan.md Phase 2, step 5).
.class public HelloJasmin
.super java/lang/Object

.method public <init>()V
    .limit stack 1
    .limit locals 1
    aload_0
    invokespecial java/lang/Object/<init>()V
    return
.end method

.method public static main([Ljava/lang/String;)V
    .limit stack 2
    .limit locals 1
    getstatic java/lang/System/out Ljava/io/PrintStream;
    ldc "jasmin ok"
    invokevirtual java/io/PrintStream/println(Ljava/lang/String;)V
    return
.end method
