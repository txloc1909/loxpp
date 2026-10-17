package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

/**
 * {@link LoxRuntime#ERROR_CLASS} is one class object that every worker
 * shares, so a write to it would be mutable state shared between threads
 * (spec/04-semantics.md, Workers and Tasks). Nothing may change it after it
 * is made: not a method definition, not an inherit.
 */
public final class ErrorClassTest {
    public static void main(String[] args) {
        LoxGlobals globals = LoxRuntime.init();
        LoxClass errorClass = LoxRuntime.ERROR_CLASS;

        checkEquals("Error", errorClass.name, "the Error class is named Error");
        check(errorClass.superclass() == null, "Error has no superclass");
        check(errorClass.methods.isEmpty(), "Error has no methods");

        checkThrows(() -> errorClass.methods.put("m", null),
                    UnsupportedOperationException.class,
                    "the methods map of Error rejects a write");
        checkThrows(() -> LoxOps.defineMethod(errorClass, "m", null),
                    UnsupportedOperationException.class,
                    "defineMethod refuses to write to Error");

        LoxClass superclass = new LoxClass("Base", null);
        checkThrows(() -> errorClass.inheritFrom(superclass),
                    IllegalStateException.class,
                    "inheritFrom refuses to change Error");
        check(errorClass.superclass() == null,
              "a refused inheritFrom leaves the superclass of Error nil");

        // Error values keep their identity and their read-only two fields.
        Object error = LoxRuntime.makeError("IndexOutOfBoundsError",
                                            "List index out of bounds.");
        check(((LoxInstance)error).klass == errorClass,
              "makeError builds an instance of the one Error class");
        checkEquals("IndexOutOfBoundsError: List index out of bounds.",
                    LoxOps.stringify(error), "an Error stringifies as kind: message");
        checkEquals("Error",
                    ((LoxCallable)globals.get("type")).call(new Object[] {error}),
                    "type of an Error value");
        checkEquals("IndexOutOfBoundsError",
                    LoxOps.getProperty(error, "kind"), "an Error exposes kind");
        checkEquals("List index out of bounds.",
                    LoxOps.getProperty(error, "message"), "an Error exposes message");
        checkThrows(() -> LoxOps.setProperty(error, "kind", "Other"),
                    LoxError.class, "a write to an Error field is a fault");

        // The guard belongs to Error alone: an ordinary class stays mutable.
        LoxClass user = new LoxClass("User", null);
        LoxOps.defineMethod(user, "m", null);
        check(user.methods.containsKey("m"), "a user class accepts a method");
        LoxClass sub = new LoxClass("Sub", null);
        sub.inheritFrom(user);
        check(sub.superclass() == user, "a user class accepts a superclass");
        check(sub.methods.containsKey("m"),
              "inheritFrom copies the methods of a user class down");

        System.exit(TestSupport.finish("ErrorClassTest"));
    }
}
