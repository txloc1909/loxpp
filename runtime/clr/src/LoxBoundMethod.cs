namespace Lox;

/// <summary>A method closure paired with the receiver it was looked up on (GET_PROPERTY / GET_SUPER).</summary>
public sealed class LoxBoundMethod : ILoxCallable {
    public readonly object Receiver;
    public readonly LoxClosure Method;

    public LoxBoundMethod(object receiver, LoxClosure method) {
        Receiver = receiver;
        Method = method;
    }

    public object Call(object[] args) => Method.CallAsSelf(Receiver, args);
}
