#pragma once

// Generic chain-walker template: walks a chain of objects looking for a match.
// T: the chain node type (e.g., Compiler, ObjClass)
// Pred: a callable that takes const T* and returns true if the node matches
// Next: a callable that takes const T* and returns the next node (or nullptr)
template <typename T, typename Pred, typename Next>
inline const T* walkChain(const T* head, Pred predicate, Next nextFn) {
    const T* current = head;
    while (current != nullptr) {
        if (predicate(current)) {
            return current;
        }
        current = nextFn(current);
    }
    return nullptr;
}
