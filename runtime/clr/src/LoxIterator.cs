using System.Collections.Generic;

namespace Lox;

/// <summary>
/// Backs the GET_ITER / ITER_HAS_NEXT / ITER_NEXT protocol. List and String
/// read the live collection by cursor (a growing list is visited further,
/// as in vm.cpp's ObjIterator). A Map snapshots its keys at construction
/// for order, and records the size: a size change during the loop is an
/// error ("Map changed size during iteration."), as in vm.cpp and Python's
/// dict rule. Writing a value to a key that already exists is permitted.
/// </summary>
public sealed class LoxIterator {
    public readonly object Collection;
    private readonly List<object> m_mapKeys; // non-null only when Collection is a LoxMap
    private readonly int m_expectedMapSize; // -1 unless Collection is a LoxMap
    private int m_index;

    public LoxIterator(object collection) {
        Collection = collection;
        if (collection is LoxMap map) {
            m_mapKeys = new List<object>();
            foreach (var e in map.Entries()) {
                m_mapKeys.Add(e.Key);
            }
            m_expectedMapSize = map.Size();
        } else {
            m_mapKeys = null;
            m_expectedMapSize = -1;
        }
    }

    private void CheckMapSize() {
        if (m_mapKeys != null &&
                ((LoxMap)Collection).Size() != m_expectedMapSize) {
            throw new LoxError("Map changed size during iteration.");
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
            CheckMapSize();
            return m_index < m_mapKeys.Count;
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }

    /// <summary>Requires a preceding true HasNext(): the compiler always
    /// emits ITER_HAS_NEXT before ITER_NEXT with no user code between them,
    /// so calling Next() past the end is unreachable from a valid program.
    /// </summary>
    public object Next() {
        if (Collection is LoxList list) {
            return list.Elements[m_index++];
        }
        if (Collection is string s) {
            return s[m_index++].ToString();
        }
        if (m_mapKeys != null) {
            CheckMapSize();
            return m_mapKeys[m_index++];
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }
}
