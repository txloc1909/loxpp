using System.Collections.Generic;

namespace Lox;

/// <summary>
/// Backs the GET_ITER / ITER_HAS_NEXT / ITER_NEXT protocol. List and String
/// read the live collection by cursor (a growing list is visited further,
/// as in vm.cpp's ObjIterator). A Map instead snapshots its keys at
/// construction: vm.cpp reads a map's live bucket cursor, so a concurrent
/// write during a <c>for (var k in m)</c> loop can be visible there on the
/// native VM. No example or bootstrap program mutates a map inside its own
/// <c>for ... in</c> loop, and the spec leaves that case unspecified, so
/// the snapshot is a safe, deterministic choice rather than a matched one.
/// </summary>
public sealed class LoxIterator {
    public readonly object Collection;
    private readonly List<object> m_mapKeys; // non-null only when Collection is a LoxMap
    private int m_index;

    public LoxIterator(object collection) {
        Collection = collection;
        if (collection is LoxMap map) {
            m_mapKeys = new List<object>();
            foreach (var e in map.Entries()) {
                m_mapKeys.Add(e.Key);
            }
        } else {
            m_mapKeys = null;
        }
    }

    public bool HasNext() {
        if (Collection is LoxList list) {
            return m_index < list.Elements.Count;
        }
        if (Collection is string s) {
            return m_index < s.Length;
        }
        if (m_mapKeys != null) {
            return m_index < m_mapKeys.Count;
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }

    public object Next() {
        if (Collection is LoxList list) {
            return list.Elements[m_index++];
        }
        if (Collection is string s) {
            return s[m_index++].ToString();
        }
        if (m_mapKeys != null) {
            return m_mapKeys[m_index++];
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }
}
