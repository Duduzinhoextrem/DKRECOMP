/* Module loader: maps each generated module at its fixed base (guest address == host address), copies the
 * linked image written by tools/dkrecomp/gen.py, leaves the bss zeroed and registers the dispatch table.
 * Relocations and symbol binding were applied when the image was generated (tools/dkrecomp/link.py). */
#include "dkr/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rt_load_modules(const DkrModule *mods, size_t count, const char *dir) {
    for (size_t i = 0; i < count; i++) {
        const DkrModule *m = &mods[i];
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.img", dir, m->name);
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "loader: cannot open %s\n", path);
            return -1;
        }
        if (rt_map_fixed(m->base, m->size)) {
            fprintf(stderr, "loader: cannot map %s at %#llx+%#llx (os error %lu)\n", m->name,
                    (unsigned long long)m->base, (unsigned long long)m->size, rt_last_os_error());
            fclose(f);
            return -1;
        }
        size_t got = fread((void *)(uintptr_t)m->base, 1, (size_t)m->image_size, f);
        int extra = fgetc(f);
        fclose(f);
        if (got != m->image_size || extra != EOF) {
            fprintf(stderr, "loader: %s is not the image this build was generated for (%zu bytes read, %llu expected)\n",
                    path, got, (unsigned long long)m->image_size);
            return -1;
        }
        rt_register_table(m->table, *m->count, m->base, m->base + m->size);
        fprintf(stderr, "loader: %-8s at %#llx (%llu KiB, %zu functions)\n", m->name, (unsigned long long)m->base,
                (unsigned long long)(m->size >> 10), *m->count);
    }
    rt_set_modules(mods, count);
    return 0;
}
