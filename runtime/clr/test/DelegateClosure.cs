using System;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// A LoxClosure test double whose body is a plain delegate, so a test can
/// build a closure inline instead of declaring a one-off subclass for each
/// case.
/// </summary>
public sealed class DelegateClosure : LoxClosure {
    private readonly Func<object, object[], object> m_invoke;

    public DelegateClosure(string name, int arity, object[][] upvalues, Func<object, object[], object> invoke)
        : base(name, arity, upvalues) {
        m_invoke = invoke;
    }

    protected override object Invoke(object self, object[] a) => m_invoke(self, a);
}
