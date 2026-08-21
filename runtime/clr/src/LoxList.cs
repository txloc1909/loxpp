using System.Collections.Generic;

namespace Lox;

/// <summary>
/// Identity equality (Lox default for List) is <see cref="object"/>'s
/// default - do not add Equals/GetHashCode. Wraps a <see cref="List{T}"/>,
/// never a bare <c>object[]</c> - see the BINDING INVARIANT at
/// <see cref="LoxOps.GetIndex"/>.
/// </summary>
public sealed class LoxList {
    public readonly List<object> Elements = new();
}
