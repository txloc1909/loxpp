using System.Collections.Generic;

namespace Lox;

/// <summary>
/// Globals are one dynamic name-to-value map (design decision A2), not one
/// static field per global - GET_GLOBAL/SET_GLOBAL are late-bound by name
/// at every access, exactly as in the native VM's hash-table globals.
/// </summary>
public sealed class LoxGlobals {
    // Stands in for Lox nil inside the table, so a stored entry is never a
    // real null. That makes "absent" (Dictionary.TryGetValue fails) and
    // "defined as nil" distinguishable, instead of a ContainsKey call
    // followed by a second lookup - GET_GLOBAL is the hottest opcode in the
    // self-hosted interpreter.
    private static readonly object s_nil = new();
    private readonly Dictionary<string, object> m_values = new();

    public void Define(string name, object value) {
        m_values[name] = Box(value);
    }

    public object Get(string name) {
        if (!m_values.TryGetValue(name, out object v)) {
            throw new LoxError($"Undefined variable '{name}'.");
        }
        return Unbox(v);
    }

    /// <summary>Matches vm.cpp's SET_GLOBAL: write speculatively, then undo and throw if the name was never defined.</summary>
    public void Set(string name, object value) {
        bool isNewKey = !m_values.ContainsKey(name);
        m_values[name] = Box(value);
        if (isNewKey) {
            m_values.Remove(name);
            throw new LoxError($"Undefined variable '{name}'.");
        }
    }

    /// <summary>Non-throwing existence check - INSTANCEOF looks up its class name this way, not via Get().</summary>
    public bool IsDefined(string name) => m_values.ContainsKey(name);

    private static object Box(object loxValue) => loxValue ?? s_nil;

    private static object Unbox(object stored) => ReferenceEquals(stored, s_nil) ? null : stored;
}
