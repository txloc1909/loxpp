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

## `str(value) → String`

Converts `value` to its canonical string representation (the same text that
`print` would output) and returns it as a String value.

```lox
print str(42);      // 42
print str(true);    // true
print str(nil);     // nil
print str(3.14);    // 3.14
```

This is the only built-in mechanism for converting a non-String value to a
String so that it can be concatenated with `+`.

**Arity:** 1  
**Returns:** String
