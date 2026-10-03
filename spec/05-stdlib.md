# Lox++ Standard Library

The following functions are available in the global scope at the start of
every program. They behave like user-defined functions in all respects
(first-class values, strict arity enforcement) except that their bodies are
provided by the host environment.

The standard library also provides the global `math` object (numeric functions
and constants), the `open` function, and the methods on the File value it
returns. These are described below. The built-in methods on Map values are
specified in §03-types and §04-semantics.

---

## `clock() → Number`

Returns the elapsed processor time in seconds as a Number.

The reference epoch (the point in time corresponding to `0`) is unspecified
and implementation-defined. The intended use is measuring elapsed time between
two calls:

```lox
var start = clock();
// ... work ...
var elapsed = clock() - start;
print str(elapsed) + " seconds";
```

**Arity:** 0  
**Returns:** Number

---

## `input() → String | Nil`

Reads one line of text from standard input and returns it as a String, with
the trailing newline character removed.

Returns `nil` if the end of input has been reached (EOF).

```lox
var line = input();
if (line == nil) {
    print "No more input.";
} else {
    print "You typed: " + line;
}
```

**Arity:** 0  
**Returns:** String on success, Nil on EOF

---

## `str(value) → String`

`str` is a keyword operator, not a global function (see §01-lexical and
§04-semantics). `str(value)` converts `value` to its canonical string
representation (the same text that `print` would output) and returns it as a
String value.

```lox
print str(42);      // 42
print str(true);    // true
print str(nil);     // nil
print str(3.14);    // 3.14

class Dog {}
print str(Dog);         // Dog
var d = Dog();
print str(d);           // Dog instance
```

For Class values, `str()` returns the class name. For Instance values, it
returns `"ClassName instance"`, or the value of the class's `__str__` method
when it defines one. For BoundMethod and native function values, it returns
`"<fn name>"` or `"<native fn>"` respectively, matching the canonical forms
documented in §03-types.

This is the only built-in mechanism for converting a non-String value to a
String so that it can be concatenated with `+`.

**Arity:** 1  
**Returns:** String

---

## `ord(value) → Number`

Returns the byte value of a one-byte String. `value` must be a String of
exactly one byte; the result is that byte's numeric value, from 0 through 255.

```lox
print ord("A");     // 65
print ord("\n");    // 10
```

A non-String argument, or a String whose length is not 1, is a runtime error.

**Arity:** 1  
**Returns:** Number

---

## `chr(value) → String`

Returns the one-byte String whose byte value is `value`. `value` must be an
integer Number from 0 through 255.

```lox
print chr(65);      // A
print len(chr(0));  // 1
```

A non-Number argument, a fractional Number, or a Number outside 0 through 255
is a runtime error. `chr` is the inverse of `ord` on every valid value:
`ord(chr(n))` equals `n`, and `chr(ord(s))` equals `s` for every one-byte
String `s`.

**Arity:** 1  
**Returns:** String

---

## `math` — numeric functions and constants

`math` is a global object bound at the start of every program. Its members are
numeric functions and constants, reached with `.`:

```lox
var h = math.hypot(3, 4);   // 5
print math.floor(2.7);      // 2
```

`math` behaves like any other global name: a local variable of the same name
shadows it, and the global can be reassigned.

### Functions of one argument

Each takes one Number and returns a Number. A non-Number argument is a runtime
error.

`abs`, `ceil`, `floor`, `round`, `sqrt`, `cbrt`, `exp`, `log`, `log2`,
`log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`

- `round` rounds a halfway value away from zero.
- `log` is the natural logarithm (base e); `log2` and `log10` use base 2 and
  base 10.
- The trigonometric functions use radians.
- A domain error, such as `math.sqrt(-1)`, returns a NaN value. It is not an
  error. The sign of that NaN and its identity are not guaranteed: the result
  can print as `-nan`, and no NaN is equal to `math.nan` or to itself.

**Arity:** 1  
**Returns:** Number

### Functions of two arguments

Each takes two Numbers and returns a Number. A non-Number argument is a runtime
error.

`pow`, `atan2`, `hypot`, `min`, `max`

- `min` and `max` take exactly two arguments and return the smaller or larger
  of the two.
- If one argument to `min` or `max` is `nan`, the other argument is returned.

**Arity:** 2  
**Returns:** Number

### Constants

| Member | Value |
|---|---|
| `math.pi` | ratio of a circle's circumference to its diameter |
| `math.e` | base of the natural logarithm |
| `math.inf` | positive infinity |
| `math.nan` | IEEE 754 quiet NaN |

---

## `args() → List[String]`

Returns the command-line arguments that followed the program's file name, as a
List of Strings. The list is empty when the program was run without arguments.

```lox
// loxpp prog.lox alpha beta
print args();   // [alpha, beta]
```

**Arity:** 0  
**Returns:** List of String

---

## `env(name) → String | Nil`

Returns the value of the environment variable `name` as a String. Returns `nil`
if the variable is not defined.

**Arity:** 1  
**Returns:** String on success, Nil if undefined

---

## `exit(code)`

Terminates the program immediately with exit code `code`.

The numeric value of `code` is truncated toward zero to an integer, matching
C's integral conversion. `code` must be a finite Number within the range of a
signed 32-bit integer (`-2147483648` to `2147483647`); any other value is a
runtime error.

**Arity:** 1  
**Returns:** never returns

---

## `time() → Number`

Returns the current calendar time as seconds since the Unix epoch (January 1,
1970 00:00:00 UTC), as a Number.

```lox
var start = time();
// ... work ...
var elapsed = time() - start;
```

Unlike `clock()`, which measures time elapsed while the program runs, `time()`
returns a value anchored to the Unix epoch — so it can be used to read the
calendar date, while `clock()` is for measuring durations.

The precision of the returned value is not part of the contract: an
implementation may report whole seconds, milliseconds, or finer.

**Arity:** 0  
**Returns:** Number

---

## `sleep(seconds) → Nil`

Suspends the program for at least `seconds` seconds. `seconds` may be a
fractional Number.

**Arity:** 1  
**Returns:** Nil

---

## `exists(path) → Bool`

Returns `true` if the file or directory at `path` exists.

**Arity:** 1  
**Returns:** Bool

---

## `is_dir(path) → Bool`

Returns `true` if `path` exists and is a directory.

**Arity:** 1  
**Returns:** Bool

---

## `is_file(path) → Bool`

Returns `true` if `path` exists and is a regular file.

**Arity:** 1  
**Returns:** Bool

---

## `stat(path) → Map | Nil`

Returns a Map describing the file or directory at `path`, or `nil` if `path`
does not exist. The Map contains the following keys:

- `exists`: `true`
- `is_dir`: `true` if `path` is a directory
- `is_file`: `true` if `path` is a regular file
- `size`: size in bytes (present only for regular files)
- `mtime`: last modification time in seconds since the Unix epoch

The precision of `mtime` is not part of the contract; an implementation may
report whole seconds, milliseconds, or finer.

**Arity:** 1  
**Returns:** Map on success, Nil if `path` does not exist

---

## `open(path, mode) → File`

Opens the file at `path` and returns a File value. Both arguments must be
Strings; any other type is a runtime error.

`mode` selects the access:

| Mode | Access | Notes |
|---|---|---|
| `"r"` | read | The file must exist. |
| `"w"` | write | Truncates the file if it exists; creates it if it does not. |
| `"a"` | write | Appends to the file; creates it if it does not exist. |
| `"r+"` | read and write | The file must exist. |

Any other mode string is a runtime error. If the file cannot be opened, for
example a missing file in mode `"r"`, `open` raises a runtime error.

**Arity:** 2  
**Returns:** File

---

## File methods

A File value is returned by `open`. `type(f)` on a File returns `"File"`.

A read method on a File that is not open for reading, and a write method on a
File that is not open for writing, are runtime errors. Every method except
`close`, called on a closed File, is a runtime error.

### `f.read() → String`

Reads the rest of the file content and returns it as one String.

**Arity:** 0  
**Returns:** String

### `f.readline() → String | Nil`

Reads the next line and returns it as a String with the trailing newline
removed. Returns `nil` at end of file.

**Arity:** 0  
**Returns:** String on success, Nil at end of file

### `f.readlines() → List[String]`

Reads the rest of the file and returns a List of its lines, each with the
trailing newline removed. A final line with no newline is included.

**Arity:** 0  
**Returns:** List of String

### `f.write(text) → Nil`

Writes `text` to the file. No newline is added. `text` must be a String.

**Arity:** 1  
**Returns:** Nil

### `f.writeline(text) → Nil`

Writes `text` followed by one newline character. `text` must be a String.

**Arity:** 1  
**Returns:** Nil

### `f.close() → Nil`

Closes the file. A second call on a File that is already closed does nothing.

**Arity:** 0  
**Returns:** Nil

### Visibility and durability

Writes may buffer. `close()` makes everything written through that File
visible to every later `open()` and to other processes. What another handle
sees before `close()` is implementation-defined and may be none, part, or all
of the data. A program that needs a second handle to see the data closes the
File first, or uses `defer f.close()`.

When the program ends — by the end of the script, by `exit()`, or by an
uncaught `throw` — every File that is still open is flushed as if `close()`
was called.

---

## `connect(host, port) -> Socket`

Opens a TCP connection to `host` at `port`, and returns a connected Socket.
Both arguments are required. `host` must be a String — a host name or a
numeric address. `port` must be an integer Number from 1 through 65535. Any
other argument is a runtime error, as is a failure to resolve the name or to
connect, which includes a refused connection.

**Arity:** 2  
**Returns:** Socket

---

## `listen(host, port) -> Server`

Binds a listening TCP socket to `host` at `port`, and returns a Server. Both
arguments are required. `host` must be a String naming a local address.
`port` must be an integer Number from 0 through 65535; `0` asks the operating
system to choose an unused port, which `Server.port()` then reports. Any other
argument is a runtime error, as is a failure to bind or to listen.

**Arity:** 2  
**Returns:** Server

---

## `spawn(program, args) -> Process`

Starts `program` as a child process, and returns a Process immediately, without
waiting for it to finish. `program` must be a String. `args` must be a List of
Strings; it holds only the arguments that follow the program name, not the
program name itself.

No shell runs between the program and the child. `program` is executed
directly, and the operating system's own search path is used when it is not an
absolute path.

A failure to start the program is a runtime error. A child that starts and then
exits with a non-zero status is not an error.

**Arity:** 2  
**Returns:** Process

---

## `run(program, args) -> Map`

Runs `program` with `args` to completion, reads all of its standard output and
standard error, and returns a Map with these keys:

- `status`: the child's exit status as a Number; the same value `Process.wait`
  returns.
- `stdout`: everything the child wrote to standard output, as a String.
- `stderr`: everything the child wrote to standard error, as a String.

Standard output and standard error are read at the same time, so a child that
writes a large amount to both cannot deadlock. `run` does not feed the child
standard input; it closes the child's standard input at once, so a child that
reads it sees an end-of-file. A failure to start the program is a runtime
error. A non-zero exit status is not an error.

**Arity:** 2  
**Returns:** Map

---

## Socket methods

A Socket is returned by `connect`, or by `Server.accept()`. `type(s)` on a
Socket returns `"Socket"`. A Socket is a bidirectional stream of bytes.

Reading a closed Socket, or writing to a Socket after `close_write()` or
`close()`, is a runtime error. A write that the peer has stopped reading also
fails with a runtime error; it never ends the program by a signal.

### `s.read() -> String`

Reads from the Socket until the peer closes its write direction, and returns
everything read as one String.

**Arity:** 0  
**Returns:** String

### `s.readline() -> String | Nil`

Reads bytes up to and including the next newline, and returns them without the
newline. Returns `nil` when the peer has closed and no bytes remain. A final
line with no newline is returned as a line.

**Arity:** 0  
**Returns:** String on success, Nil at end of stream

### `s.readlines() -> List[String]`

Reads lines as `readline` does until the peer closes, and returns them as a
List.

**Arity:** 0  
**Returns:** List of String

### `s.write(text) -> Nil`

Writes `text`. `text` must be a String.

**Arity:** 1  
**Returns:** Nil

### `s.writeline(text) -> Nil`

Writes `text` followed by one newline. `text` must be a String.

**Arity:** 1  
**Returns:** Nil

### `s.close_write() -> Nil`

Closes the Socket's write direction only, which sends an end-of-file to the
peer. Reading stays possible. A second call, and a call after `close()`, do
nothing.

**Arity:** 0  
**Returns:** Nil

### `s.close() -> Nil`

Closes the Socket in both directions. A second call does nothing.

**Arity:** 0  
**Returns:** Nil

---

## Server methods

A Server is returned by `listen`. `type(s)` on a Server returns `"Server"`.

### `s.accept() -> Socket`

Waits for the next inbound connection, and returns it as a new Socket. Calling
`accept()` on a closed Server is a runtime error.

**Arity:** 0  
**Returns:** Socket

### `s.port() -> Number`

The local port the Server is bound to. When `listen` was given port `0`, this
reports the port the operating system chose.

**Arity:** 0  
**Returns:** Number

### `s.close() -> Nil`

Stops listening. A connection already accepted stays usable. A second call does
nothing.

**Arity:** 0  
**Returns:** Nil

---

## Process methods

A Process is returned by `spawn`. `type(p)` on a Process returns `"Process"`.

Reading a closed Process pipe, or writing to it after `close_stdin()` or after
the process has been waited on, is a runtime error. A write to a child that
has stopped reading also fails with a runtime error; it never ends the program
by a signal.

A child's standard output and standard error are two separate streams. Reading
only one of them while the child writes a large amount to the other can block
the child. Read both, or use `run`.

### `p.read() -> String`, `p.readline() -> String | Nil`, `p.readlines() -> List[String]`

Read the child's standard output. These behave exactly like the Socket methods
of the same name.

### `p.read_err() -> String`, `p.err_readline() -> String | Nil`, `p.err_readlines() -> List[String]`

Read the child's standard error. These behave exactly like the Socket methods
of the same name.

### `p.write(text) -> Nil`, `p.writeline(text) -> Nil`

Write to the child's standard input. `text` must be a String.

### `p.close_stdin() -> Nil`

Closes the child's standard input, which sends it an end-of-file. A second call
does nothing.

**Arity:** 0  
**Returns:** Nil

### `p.wait() -> Number`

Waits for the child to exit, and returns its exit status as a Number. The
status is the child's exit code when it exited normally, or `128 + signal` when
it was terminated by a signal. `wait()` blocks. A second call returns the same
status at once. Every still-running child should be waited on, or it can stay a
zombie until the program ends.

**Arity:** 0  
**Returns:** Number

### `p.kill() -> Nil`

Terminates the child at once. This does not wait; call `wait()` to reap the
child and get its status.

**Arity:** 0  
**Returns:** Nil

### `p.pid() -> Number`

The child's process identifier.

**Arity:** 0  
**Returns:** Number

---

## Map methods

Map values respond to the built-in methods `has`, `del`, `keys`, `values`, and
`entries`. They are specified in §03-types (summary) and §04-semantics
(evaluation rules), and are not repeated here.

---

## `type(value) → String`

Returns the language-level type name of `value` as a String.

| `value` is a... | Returns |
|---|---|
| Nil | `"Nil"` |
| Boolean | `"Boolean"` |
| Number | `"Number"` |
| String | `"String"` |
| Function (see §03-types) | `"Function"` |
| BoundMethod | `"BoundMethod"` |
| Class | `"Class"` |
| Instance | `"Instance"` |
| List | `"List"` |
| Map | `"Map"` |
| File | `"File"` |
| Socket | `"Socket"` |
| Server | `"Server"` |
| Process | `"Process"` |
| Error (see §03-types) | `"Error"` |
| Iterator | `"Iterator"` |
| Enum constructor | `"EnumConstructor"` |
| Enum value | `"Enum"` |

`type()` reports the same names `str()` and the rest of §03-types use for
these values — it does not distinguish a native function from a user-defined
one, or a bound method backed by a native from one backed by a user-defined
method; both count as `Function` and `BoundMethod` respectively.

No Lox++ program can currently obtain an Iterator value directly — `for-in`
consumes one internally, but never exposes it as a variable's value. The
`"Iterator"` row exists for completeness of the runtime's type space, not as
a promise that a program can produce one to pass to `type()` today.

```lox
print type(1);          // Number
print type("hi");       // String
class Dog {}
print type(Dog);        // Class
print type(Dog());      // Instance
```

**Arity:** 1  
**Returns:** String

---

## `fields(instance) → List[String]`

Returns the names of every field currently set on `instance`, as a List of
Strings. Only the instance's own field table is enumerated — inherited or own
**methods** are not fields and never appear in this list, even though `.`
property access can reach them (see §04-semantics, Property Get).

The order of the returned names is unspecified. Sort the result if a
deterministic order is needed.

Runtime error ("Expected an instance.") if `instance` is not an Instance.

**Arity:** 1  
**Returns:** List of String

---

## `methods(class) → List[String]`

Returns the names of every method `class` responds to, as a List of Strings —
its own methods plus every method inherited from a superclass.

The order of the returned names is unspecified. Sort the result if a
deterministic order is needed.

Runtime error ("Expected a class.") if `class` is not a Class.

**Arity:** 1  
**Returns:** List of String

---

## `getField(instance, name) → Any`

Returns the value of the field named `name` on `instance`.

This is deliberately **fields-only**: unlike `.` property access (§04-
semantics, Property Get), `getField` never falls back to a bound method when
`name` is not a field. `getField(instance, name)` reflects exactly what
`fields(instance)` enumerates — if `name` does not appear in `fields
(instance)`, `getField` returns `nil`, even if `instance`'s class defines a
method by that name.

Returns `nil` if `instance` has no field named `name` (use `hasField` to
distinguish "absent" from "present and nil").

Runtime error ("Only instances have properties.") if `instance` is not an
Instance.

**Arity:** 2  
**Returns:** the field's value, or Nil if absent

---

## `hasField(instance, name) → Boolean`

Returns `true` if `instance` has a field named `name` — equivalently, whether
`name` appears in `fields(instance)`. As with `getField`, this checks the
field table only; a method named `name` does not make `hasField` return
`true`.

Runtime error ("Only instances have properties.") if `instance` is not an
Instance.

**Arity:** 2  
**Returns:** Boolean

---

## `setField(instance, name, value) → Any`

Sets the field named `name` on `instance` to `value`, creating the field if it
does not already exist. Behaves exactly like `.` property assignment
(§04-semantics, Property Set) driven by a runtime-computed name instead of a
compile-time identifier.

Returns `value` (assignment is an expression, matching `.` assignment).

Runtime error ("Only instances have fields.") if `instance` is not an
Instance.

**Arity:** 3  
**Returns:** the assigned value

---

## `callMethod(instance, name, ...args) → Any`

Calls the method or callable field named `name` on `instance` with `args`,
and returns its result. Resolution matches Method Invocation (§04-semantics):
if `instance`'s field table contains `name`, that value is called; otherwise
the method named `name` on `instance`'s class (including inherited methods)
is called.

A method resolved from the class binds `this` to `instance`, exactly as an
ordinary `instance.name(...)` call does. A function value stored in a field
is called as-is (slot 0 is the function itself, as for a plain call).

**Restriction:** a Class or Enum constructor value is callable via `()`, but
`callMethod` does not support it — calling one raises a **runtime error**
("callMethod does not support class or enum constructor values."). Any other
non-callable value raises a runtime error
("callMethod requires a callable value.").

```lox
class Foo {}
var f = Foo();
f.describe = str;      // a native function stored in a field
print callMethod(f, "describe", 42);   // "42"

class Bar { greet() { return "hi"; } }
var b = Bar();
print callMethod(b, "greet");          // "hi"
```

Runtime error ("Only instances have methods.") if `instance` is not an
Instance. Runtime error ("Undefined property 'name'.") if `name` names
neither a field nor a method. Runtime error if the resolved callable is
called with the wrong number of arguments, matching ordinary call-arity
checking. A `throw` inside the called body propagates as an ordinary throw:
an enclosing `try` catches it.

**Arity:** variadic (at least 2: `instance` and `name`)  
**Returns:** the called method's result
