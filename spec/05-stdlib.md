# Lox++ Standard Library

The following functions are available in the global scope at the start of
every program. They behave like user-defined functions in all respects
(first-class values, strict arity enforcement) except that their bodies are
provided by the host environment.

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

## `len(seq) → Number`

Returns the number of elements in `seq`.

- If `seq` is a **List**: returns the number of elements.
- If `seq` is a **String**: returns the number of bytes.
- If `seq` is a **Map**: returns the number of key-value pairs.

Runtime error if `seq` is any other type.

```lox
print len([1, 2, 3]);          // 3
print len("hello");            // 5
print len([]);                 // 0
print len("");                 // 0
print len({"a": 1, "b": 2});   // 2
print len({});                 // 0
```

**Arity:** 1  
**Returns:** Number

---

## `str(value) → String`

Converts `value` to its canonical string representation (the same text that
`print` would output) and returns it as a String value.

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
returns `"ClassName instance"`. For BoundMethod and native function values, it
returns `"<fn name>"` or `"<native fn>"` respectively, matching the canonical
forms documented in §03-types.

This is the only built-in mechanism for converting a non-String value to a
String so that it can be concatenated with `+`.

**Arity:** 1  
**Returns:** String

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
