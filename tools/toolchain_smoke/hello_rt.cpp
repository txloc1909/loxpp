// Hand-written probe for the QBE assemble -> link -> run chain, before any
// code generator exists. tools/toolchain_smoke/HelloRt.j is the JVM
// equivalent; this mirrors it: a tiny C++ static library
// standing in for libloxrt.a, called from QBE-compiled code through the C
// ABI (extern "C").
#include <cstdio>

extern "C" int lox_rt_hello() {
    std::printf("qbe ok\n");
    return 0;
}
