package lox;

import static lox.TestSupport.checkEquals;

import java.io.File;
import java.io.IOException;

/**
 * Every number case here was captured from build/loxpp itself (the oracle —
 * see RT.md) via a probe script printing the same 21 values, run inside the
 * dev-managed container. LoxOps.formatNumber must match byte for byte.
 */
public final class StringifyTest {
    public static void main(String[] args) throws IOException {
        checkEquals("0", LoxOps.stringify(0.0), "stringify(0)");
        checkEquals("1", LoxOps.stringify(1.0), "stringify(1)");
        checkEquals("-1", LoxOps.stringify(-1.0), "stringify(-1)");
        checkEquals("3", LoxOps.stringify(3.0), "stringify(3)");
        checkEquals("3.5", LoxOps.stringify(3.5), "stringify(3.5)");
        checkEquals("0.1", LoxOps.stringify(0.1), "stringify(0.1)");
        checkEquals("0.5", LoxOps.stringify(0.5), "stringify(0.5)");
        checkEquals("100", LoxOps.stringify(100.0), "stringify(100)");
        checkEquals("1e+06", LoxOps.stringify(1000000.0), "stringify(1000000)");
        checkEquals("999999", LoxOps.stringify(999999.0), "stringify(999999)");
        checkEquals("1.23457e+06", LoxOps.stringify(1234567.0), "stringify(1234567)");
        checkEquals("0.0001", LoxOps.stringify(0.0001), "stringify(0.0001)");
        checkEquals("1e-05", LoxOps.stringify(0.00001), "stringify(0.00001)");
        checkEquals("1e+20", LoxOps.stringify(Math.pow(10, 20)), "stringify(1e20)");
        checkEquals("1e-20", LoxOps.stringify(Math.pow(10, -20)), "stringify(1e-20)");
        checkEquals("-0", LoxOps.stringify(-0.0), "stringify(-0.0)");
        // A runtime (not compile-time-folded) division, exactly as
        // LoxOps.divide always performs it: on x86_64 this carries a set
        // sign bit, matching glibc's "-nan" for the native oracle.
        checkEquals("-nan", LoxOps.stringify(LoxOps.divide(0.0, 0.0)), "stringify(0/0)");
        checkEquals("inf", LoxOps.stringify(LoxOps.divide(1.0, 0.0)), "stringify(1/0)");
        checkEquals("2.5", LoxOps.stringify(2.5), "stringify(2.5)");
        checkEquals("0.333333", LoxOps.stringify(LoxOps.divide(1.0, 3.0)), "stringify(1/3)");
        checkEquals("1.23457e+08", LoxOps.stringify(123456789.0), "stringify(123456789)");

        checkEquals("true", LoxOps.stringify(true), "stringify(true)");
        checkEquals("false", LoxOps.stringify(false), "stringify(false)");
        checkEquals("nil", LoxOps.stringify(null), "stringify(nil)");
        checkEquals("hello", LoxOps.stringify("hello"), "stringify(string)");

        LoxClosure fn = new LoxClosure("greet", 0, new Object[0][]) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return null;
            }
        };
        checkEquals("<fn greet>", LoxOps.stringify(fn), "stringify(named closure)");
        LoxClosure script = new LoxClosure(null, 0, new Object[0][]) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return null;
            }
        };
        checkEquals("<script>", LoxOps.stringify(script), "stringify(script closure)");
        checkEquals("<native fn>", LoxOps.stringify(new LoxNative("clock", 0, a -> null)),
                "stringify(native)");

        LoxClass klass = new LoxClass("Dog", null);
        checkEquals("Dog", LoxOps.stringify(klass), "stringify(class)");
        LoxInstance instance = new LoxInstance(klass);
        checkEquals("Dog instance", LoxOps.stringify(instance), "stringify(instance)");
        checkEquals("<fn greet>", LoxOps.stringify(new LoxBoundMethod(instance, fn)),
                "stringify(bound method)");

        LoxList list = new LoxList();
        checkEquals("[]", LoxOps.stringify(list), "stringify(empty list)");
        list.elements.add(1.0);
        list.elements.add("hello");
        list.elements.add(true);
        list.elements.add(null);
        checkEquals("[1, hello, true, nil]", LoxOps.stringify(list), "stringify(list)");

        LoxMap map = new LoxMap();
        checkEquals("{}", LoxOps.stringify(map), "stringify(empty map)");
        map.put("a", 1.0);
        map.put("b", 2.0);
        checkEquals("{a: 1, b: 2}", LoxOps.stringify(map), "stringify(map)");

        LoxEnumCtor ctor = new LoxEnumCtor(1, 2, "Ok", "Result");
        checkEquals("<ctor Result::Ok>", LoxOps.stringify(ctor), "stringify(enum ctor)");
        checkEquals("Result::Ok(1, 2)",
                LoxOps.stringify(new LoxEnum(ctor, new Object[] {1.0, 2.0})), "stringify(enum with payload)");
        LoxEnumCtor nullary = new LoxEnumCtor(0, 0, "None", "Option");
        checkEquals("Option::None", LoxOps.stringify(new LoxEnum(nullary, new Object[0])),
                "stringify(nullary enum)");

        checkEquals("<iterator>", LoxOps.stringify(LoxOps.getIter(list)), "stringify(iterator)");

        File tmp = File.createTempFile("lox-rt-stringify", ".txt");
        tmp.deleteOnExit();
        LoxFile file = LoxFile.open(tmp.getAbsolutePath(), "w");
        checkEquals("<file>", LoxOps.stringify(file), "stringify(file)");
        file.close();

        System.exit(TestSupport.finish("StringifyTest"));
    }
}
