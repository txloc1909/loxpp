// Minimal ilasm input: exercises the same .il -> .dll -> dotnet path the CLR
// backend will use.
//
// CoreCLR has no single mscorlib to reference the way .NET Framework did, so
// System.Object and System.Console must be pulled from their own assemblies.
// The generated backend will need to emit these same extern declarations.
.assembly extern System.Runtime { .ver 8:0:0:0 }
.assembly extern System.Console { .ver 8:0:0:0 }
.assembly HelloIl {}
.module HelloIl.dll

.class public auto ansi HelloIl extends [System.Runtime]System.Object
{
  .method public static void Main() cil managed
  {
    .entrypoint
    .maxstack 1
    ldstr "ilasm ok"
    call void [System.Console]System.Console::WriteLine(string)
    ret
  }
}
