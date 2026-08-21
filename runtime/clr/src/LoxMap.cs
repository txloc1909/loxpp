using System.Collections.Generic;

namespace Lox;

/// <summary>
/// <c>Dictionary&lt;object, object&gt;</c> throws on a null key (unlike
/// Java's HashMap), and nil is a legal Lox map key, so nil is stored under
/// <see cref="s_nilKey"/> instead of a real null.
///
/// Iteration follows insertion order (see <see cref="m_order"/>): spec/03-
/// types.md leaves map iteration order unspecified, and the native ObjMap
/// gives bucket order instead, so the two runtimes may print <c>keys()</c>
/// in different orders on the same program. That gap is not a defect
/// (matches the ruling recorded for the JVM runtime's own LinkedHashMap
/// choice); insertion order is picked here because it is stable within one
/// run, which a permutation proof needs.
///
/// Key validity (nil/bool/number-not-NaN/string only) is enforced by
/// callers (<see cref="LoxOps"/>), matching vm.cpp: the check happens at
/// each opcode site, not inside LoxMap itself.
/// </summary>
public sealed class LoxMap {
    // Stands in for a Lox nil key: Dictionary<object,object> rejects a real
    // null key outright.
    private static readonly object s_nilKey = new();

    private sealed class Slot {
        public readonly object DisplayKey;
        public readonly object Value;

        public Slot(object displayKey, object value) {
            DisplayKey = displayKey;
            Value = value;
        }
    }

    // Keyed by the numerically-normalized key, so -0.0 and 0.0 land in the
    // same slot (matching value.cpp's hashValue). Each Slot separately holds
    // the exact key object the caller last wrote: CoreHashMap::set replaces
    // the whole entry, key included, on a repeat write, not only the value,
    // so a later write under -0.0 must still print as -0, even though it
    // looks up the same slot as 0.0.
    private readonly Dictionary<object, Slot> m_entries = new();

    // Insertion order of the live keys, kept alongside m_entries so
    // iteration order is deterministic within one run (see class summary).
    private readonly List<object> m_order = new();

    private static object NormalizeKey(object key) {
        if (key == null) {
            return s_nilKey;
        }
        // -0.0 and 0.0 must hash and look up identically, matching IEEE 754
        // numeric equality (value.cpp's hashValue canonicalizes the same
        // way). This normalization does not depend on how the CLR's own
        // Double.Equals/GetHashCode treat signed zero.
        if (key is double d && d == 0.0) {
            return 0.0;
        }
        return key;
    }

    public void Put(object key, object value) {
        object normalized = NormalizeKey(key);
        // A repeat write still replaces the displayed key (e.g. a later
        // write under -0.0 must display as -0, not the earlier 0.0), so the
        // whole Slot is replaced rather than only its value.
        bool isNewKey = !m_entries.ContainsKey(normalized);
        m_entries[normalized] = new Slot(key, value);
        if (isNewKey) {
            m_order.Add(normalized);
        }
    }

    public object Get(object key) {
        return m_entries.TryGetValue(NormalizeKey(key), out Slot slot) ? slot.Value : null;
    }

    public bool Has(object key) {
        return m_entries.ContainsKey(NormalizeKey(key));
    }

    public void Remove(object key) {
        object normalized = NormalizeKey(key);
        if (m_entries.Remove(normalized)) {
            m_order.Remove(normalized);
        }
    }

    public int Size() => m_entries.Count;

    /// <summary>Insertion order, each entry's key exactly as the caller last wrote it.</summary>
    public IEnumerable<KeyValuePair<object, object>> Entries() {
        var result = new List<KeyValuePair<object, object>>(m_order.Count);
        foreach (object normalized in m_order) {
            Slot slot = m_entries[normalized];
            object displayKey = ReferenceEquals(slot.DisplayKey, s_nilKey) ? null : slot.DisplayKey;
            result.Add(new KeyValuePair<object, object>(displayKey, slot.Value));
        }
        return result;
    }

    // Every LoxMap lazily grows its own per-instance cache the first time a
    // method is read as a property (not called): the native VM hands back
    // the same ObjNative on every GET_PROPERTY, so repeated `m.has == m.has`
    // must read the identical object twice.
    private readonly Dictionary<string, ILoxCallable> m_methodCache = new();

    public ILoxCallable GetMethod(string name) {
        if (m_methodCache.TryGetValue(name, out ILoxCallable cached)) {
            return cached;
        }
        ILoxCallable created = CreateMethod(name);
        if (created != null) {
            m_methodCache[name] = created;
        }
        return created;
    }

    private ILoxCallable CreateMethod(string name) {
        switch (name) {
        case "has":
            return new LoxNative("has", 1, a => {
                LoxOps.CheckMapKey(a[0]);
                return Has(a[0]);
            });
        case "del":
            return new LoxNative("del", 1, a => {
                LoxOps.CheckMapKey(a[0]);
                Remove(a[0]);
                return null;
            });
        case "keys":
            return new LoxNative("keys", 0, a => {
                var list = new LoxList();
                foreach (var e in Entries()) {
                    list.Elements.Add(e.Key);
                }
                return list;
            });
        case "values":
            return new LoxNative("values", 0, a => {
                var list = new LoxList();
                foreach (var e in Entries()) {
                    list.Elements.Add(e.Value);
                }
                return list;
            });
        case "entries":
            return new LoxNative("entries", 0, a => {
                var list = new LoxList();
                foreach (var e in Entries()) {
                    var pair = new LoxList();
                    pair.Elements.Add(e.Key);
                    pair.Elements.Add(e.Value);
                    list.Elements.Add(pair);
                }
                return list;
            });
        default:
            return null;
        }
    }
}
