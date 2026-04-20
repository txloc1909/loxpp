# CLR Backend Plan

Adds a `--target clr` compilation path to loxpp. Mirrors the structure of the JVM
plan: the existing parser and compiler are left untouched; a new backend walks the
`ObjFunction` tree and translates to CIL text (`.il` files), which are assembled by
`ilasm` (ships with .NET SDK) and run with `dotnet`.

## Architecture

```
source
  └─ Scanner → Parser → Compiler (existing, unchanged)
                                └─ ObjFunction tree (Chunks)
                                        └─ ClrBackend (new)
                                                └─ .il CIL text files
                                                        └─ ilasm → .dll
                                                                └─ dotnet LoxMain.dll
```

Same Chunk-walking strategy as the JVM plan. Zero frontend changes.

## Phase 1 — C# Runtime Library (`runtime/clr/`)

Write once in C#, compiled to `LoxRuntime.dll`. All generated CIL delegates hard
semantics here.

```
runtime/clr/src/
  ILoxCallable.cs       // interface: object Call(object[] args)
  LoxClosure.cs         // abstract: implements ILoxCallable, object[][] upvals
  LoxClass.cs           // string Name, Dictionary<string,LoxClosure> Methods,
                        //   LoxClass Superclass
                        //   LoxClosure? FindMethod(string name) — walks inheritance chain
  LoxInstance.cs        // LoxClass Klass, Dictionary<string,object> Fields
                        //   GetProperty(string) / SetProperty(string, object)
                        //   same lookup order as JVM plan: fields first, then class methods
  LoxBoundMethod.cs     // object Receiver, LoxClosure Method — implements ILoxCallable
  LoxList.cs            // List<object> — implements sequence protocol
  LoxOps.cs             // static helpers for every operation (same surface as JVM plan):
                        //   Add/Subtract/Multiply/Divide/Modulo/Negate
                        //   Equal/Greater/Less/Not/In
                        //   CheckNumber(object) — throws LoxError if not double
                        //   Print(object)
                        //   Invoke(object receiver, string name, object[] args)
                        //   GetIndex/SetIndex
  LoxError.cs           // extends Exception
  LoxNative.cs          // delegate object LoxNativeFn(object[] args)
  LoxRuntime.cs         // bootstraps globals: clock(), math stdlib, etc.
```

Key design decisions:
- **All values are `object`** in CIL. Nil = `null`. Bool = boxed `bool`. Number =
  boxed `double`. String = `System.String`.
- **Upvalues** use the same ref-cell trick as the JVM plan: a single-element `object[]`
  array. Mutating an upvalue writes `cell[0]`. Generated closure classes receive
  their ref-cells in the constructor and store them in fields.
- **`LoxClosure.upvals`** is `object[][]`. The base class constructor takes and stores
  the array; generated subclasses call `base(upvals)`.
- **No DLR dependency**. The runtime is intentionally self-contained and does not
  reference `Microsoft.CSharp` or `System.Dynamic`. This keeps the dependency surface
  minimal and makes the semantics explicit. DLR could be layered in later for
  inline-caching if performance benchmarks show dispatch is the bottleneck.

Build: `dotnet build` produces `LoxRuntime.dll`.

## Phase 2 — CLR Backend (`src/clr_backend.h` / `src/clr_backend.cpp`)

```cpp
// Entry point called from main when --target clr is passed
void compileToClr(ObjFunction* toplevel, const std::string& outputDir);
```

Same two-phase structure as the JVM backend:

1. **Collect all functions** — walk ObjFunction constants recursively, assign unique
   CIL class names (`LoxFn_0`, `LoxFn_1`, etc.). Top-level script = `LoxMain` with
   `static void Main(string[])`.

2. **For each ObjFunction, emit a `.il` text segment**:
   ```
   .class public LoxFn_3 extends [LoxRuntime]Lox.LoxClosure
   {
     .method public instance void .ctor(object[][] upvals)
     {
       ldarg.0
       ldarg.1
       call instance void Lox.LoxClosure::.ctor(object[][])
       ret
     }
     .method public virtual instance object Call(object[] args)
     {
       .locals init (object[] var0, object[] var1, ...)
       // translated opcodes
     }
   }
   ```

3. **Opcode translation table**:

| Lox++ Op | CIL |
|---|---|
| `CONSTANT` (Number) | `ldc.r8 <double>` → `box float64` |
| `CONSTANT` (String) | `ldstr "<string>"` |
| `NIL` | `ldnull` |
| `TRUE` | `ldc.i4.1` → `box bool` |
| `FALSE` | `ldc.i4.0` → `box bool` |
| `ADD/SUBTRACT/…` | `call object Lox.LoxOps::Add(object, object)` |
| `NEGATE/NOT` | `call object Lox.LoxOps::Negate` / `Not` |
| `EQUAL/GREATER/LESS` | `call object Lox.LoxOps::Equal` / `Greater` / `Less` |
| `PRINT` | `call void Lox.LoxOps::Print(object)` |
| `POP` | `pop` |
| `GET_LOCAL n` | `ldloc <n>` |
| `SET_LOCAL n` | `stloc <n>` |
| `DEFINE_GLOBAL name` | `stsfld object Lox.LoxGlobals::<name>` |
| `GET_GLOBAL name` | `ldsfld object Lox.LoxGlobals::<name>` |
| `SET_GLOBAL name` | `stsfld object Lox.LoxGlobals::<name>` |
| `JUMP offset` | `br label_<target>` |
| `JUMP_IF_FALSE offset` | `call bool Lox.LoxOps::IsFalsy(object)` → `brtrue label_<target>` |
| `LOOP offset` | `br label_<target>` |
| `CALL argCount` | `castclass Lox.ILoxCallable` → `callvirt object Call(object[])` |
| `RETURN` | `ret` |
| `CLOSURE fnIdx upvals…` | `newobj LoxFn_N::.ctor(object[][])` after building upval array |
| `GET_UPVALUE n` | `ldfld object[][] LoxClosure::upvals` → `n` → `ldelem` → `ldc.i4.0` → `ldelem` |
| `SET_UPVALUE n` | same load path + `stelem` into `cell[0]` |
| `CLOSE_UPVALUE` | no-op (ref-cells are already heap objects) |
| `GET_PROPERTY name` | `castclass Lox.LoxInstance` → `callvirt GetProperty(string)` |
| `SET_PROPERTY name` | `castclass Lox.LoxInstance` → `callvirt SetProperty(string, object)` |
| `INVOKE name argc` | `call object Lox.LoxOps::Invoke(object, string, object[])` |
| `CLASS name` | `ldstr name` → `newobj Lox.LoxClass::.ctor(string)` |
| `INHERIT` | `callvirt Lox.LoxClass::Inherit(LoxClass)` |
| `DEFINE_METHOD name` | `callvirt Lox.LoxClass::DefineMethod(string, LoxClosure)` |
| `GET_SUPER name` | `call object Lox.LoxOps::GetSuper(object, LoxClass, string)` |
| `SUPER_INVOKE name argc` | `call object Lox.LoxOps::SuperInvoke(object, LoxClass, string, object[])` |
| `BUILD_LIST n` | `newobj Lox.LoxList::.ctor()` + `n` × `callvirt Add(object)` |
| `GET_INDEX` | `call object Lox.LoxOps::GetIndex(object, object)` |
| `SET_INDEX` | `call object Lox.LoxOps::SetIndex(object, object, object)` |
| `IN` | `call object Lox.LoxOps::In(object, object)` |

4. **Globals**: one `LoxGlobals.il` with a class holding `public static object <name>`
   fields. `LoxMain::Main` calls `Lox.LoxRuntime.Init()` first.

5. **Locals declaration**: CIL `.locals init` requires declaring all locals upfront.
   Scan the Chunk for max local slot index before emitting the method body.

6. **Branch labels**: same as JVM plan — pre-scan the Chunk for all jump targets,
   emit `label_<byteOffset>:` anchors during translation.

7. **`tail.` prefix**: CIL supports tail call elimination with the `tail.` prefix
   before a `call`/`callvirt`. Emit it on `CALL` + `RETURN` sequences where the
   call immediately precedes the return. Simple pattern-match on the Chunk:
   `CALL n, RETURN` → `tail. callvirt …` + `ret`.

## Phase 3 — Build Integration

```cmake
option(LOXPP_CLR_BACKEND "Build the CLR bytecode backend" OFF)
if(LOXPP_CLR_BACKEND)
    target_sources(loxpp PRIVATE src/clr_backend.cpp)
    target_compile_definitions(loxpp PRIVATE LOXPP_CLR_BACKEND)
endif()
```

CLI: `loxpp --target clr --out-dir build/clr/ program.lox`

Post-compilation:
```bash
ilasm /dll /output:build/clr/LoxMain.dll build/clr/LoxMain.il
dotnet build runtime/clr/ -o build/clr/   # puts LoxRuntime.dll next to LoxMain.dll
dotnet build/clr/LoxMain.dll
```

## File Layout

```
runtime/clr/
  src/*.cs              # runtime library source
  LoxRuntime.csproj
  LoxRuntime.dll        # built artifact
src/
  clr_backend.h
  clr_backend.cpp
```

## Differences from JVM Plan

| Concern | JVM | CLR |
|---|---|---|
| Assembler tool | `jasmin.jar` (bundled) | `ilasm` (ships with .NET SDK, no bundling) |
| Stack map frames | Jasmin computes automatically | Not required in CIL |
| Local variable decl | Implicit (JVM infers from bytecode) | Must declare upfront with `.locals init` |
| Tail calls | Not supported | `tail.` prefix — implement for `CALL+RETURN` |
| Binary format | `.class` (per-class files) | `.dll` (single assembly for all classes) |
| Upvalue ref-cells | `Object[]` | `object[]` — identical pattern |
| Arithmetic | `invokestatic LoxOps.add` | `call Lox.LoxOps::Add` — identical pattern |

## Open Questions / Risks

- **Single `.dll` vs multiple**: `ilasm` can assemble multiple `.il` files into one
  `.dll` (via `/resource` or merging). Emit all classes into one `LoxMain.il` or
  use `ILMerge`/`illink` in the build step. Single file is simpler to start.
- **`CLOSE_UPVALUE` as no-op**: same justification as JVM plan — all captured locals
  are ref-cells from declaration. Acceptable for baseline.
- **Tail call correctness**: the `tail.` prefix is a hint; the CLR JIT is not required
  to honor it (though RyuJIT does for direct calls). For the performance baseline this
  is fine. Deep recursion will still stack-overflow, same as native loxpp.
- **ilasm availability**: `ilasm` lives at
  `$(dotnet --list-runtimes | ... )/ilasm` — its path is version-dependent.
  The build script should locate it via `dotnet --list-sdks` or hardcode a
  discovery fallback.
