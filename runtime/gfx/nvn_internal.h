/* Shared by the NVN host files (nvn.c: device and proc registry; nvn_obj.c: objects). */
#ifndef DKR_NVN_INTERNAL_H
#define DKR_NVN_INTERNAL_H

#include "dkr/rt.h"
#include <stddef.h>

typedef struct NvnImpl {
    const char *name;
    GuestFn fn;
} NvnImpl;

extern const NvnImpl dkr_nvn_obj_impls[];
extern const size_t dkr_nvn_obj_impl_count;
GuestFn dkr_nvn_obj_fallback(const char *name); /* generic handler for a family (command recording), or NULL */

/* Guest address of the original pure driver routine for `name` (any category), 0 if the analysis did not list it. */
uint64_t dkr_nvn_original(const char *name);

#endif
