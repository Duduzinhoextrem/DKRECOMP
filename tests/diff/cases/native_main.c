/* Native reference for tests/diff/cases/programs.c: prints test_entry(which, seed) for each argument pair. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t test_entry(uint64_t which, uint64_t seed);

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        uint64_t which = strtoull(argv[i], NULL, 0), seed = strtoull(argv[i + 1], NULL, 0);
        printf("%llu\n", (unsigned long long)test_entry(which, seed));
    }
    return 0;
}
