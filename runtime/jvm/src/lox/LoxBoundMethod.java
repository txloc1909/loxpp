package lox;

/** A method closure paired with the receiver it was looked up on (GET_PROPERTY / GET_SUPER). */
public final class LoxBoundMethod implements LoxCallable {
    public final Object receiver;
    public final LoxClosure method;

    public LoxBoundMethod(Object receiver, LoxClosure method) {
        this.receiver = receiver;
        this.method = method;
    }

    @Override
    public Object call(Object[] args) {
        return method.callAsSelf(receiver, args);
    }
}
