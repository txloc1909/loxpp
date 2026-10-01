# Language Extension — Open Questions

## Map key constraints

Resolved by issue #469 (operator overloading D1). A Map key may be Nil, Bool,
a non-NaN Number, a String, or an Instance whose class defines both `__hash__`
and `__eq__`. The map calls `__hash__` to place the key and resolves a
collision with the stored key's `__eq__` (stored on the left, CPython style).
Two keys that compare equal must return the same `__hash__`; a class that
breaks this contract gives undefined lookup results. See `spec/03-types.md`
(Map) and `spec/04-semantics.md` (Operator Overloading).
