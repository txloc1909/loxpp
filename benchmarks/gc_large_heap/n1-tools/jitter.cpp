#include <cstdio>
#include <ctime>
#include <cstdint>
static uint64_t now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000ULL + t.tv_nsec;
}
int main() {
    uint64_t start = now(), prev = start, mx = 0;
    unsigned n100 = 0, n1000 = 0, n3000 = 0;
    while (now() - start < 8000000000ULL) {
        uint64_t t = now();
        uint64_t g = t - prev;
        prev = t;
        if (g > mx)
            mx = g;
        if (g > 100000)
            n100++;
        if (g > 1000000)
            n1000++;
        if (g > 3000000)
            n3000++;
    }
    printf("max gap %llu us; gaps >100us: %u, >1ms: %u, >3ms: %u (8 s)\n",
           (unsigned long long)mx / 1000, n100, n1000, n3000);
}
