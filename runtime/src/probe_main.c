/* Link probe for a generated module (tools/dkrecomp/gen.py): proves the lifted C compiles and links with the
 * runtime, and checks the dispatch table. It does not run any guest code; the module is not loaded. */
#include "dkr/rt.h"

#include <stdio.h>

extern const uint64_t dkr_module_base;

int main(void) {
    size_t bad = 0;
    for (size_t i = 1; i < dkr_fn_count; i++)
        if (dkr_fn_table[i - 1].addr >= dkr_fn_table[i].addr) bad++;
    for (size_t i = 0; i < dkr_fn_count; i++)
        if (rt_lookup(dkr_fn_table[i].addr) != dkr_fn_table[i].fn) bad++;
    printf("module base %#llx, %zu lifted functions, first %#llx, last %#llx, table errors %zu\n",
           (unsigned long long)dkr_module_base, dkr_fn_count, (unsigned long long)dkr_fn_table[0].addr,
           (unsigned long long)dkr_fn_table[dkr_fn_count - 1].addr, bad);
    return bad != 0;
}
