/* Immutable command arguments for the host replay backend. GPU addresses remain addresses, not CPU snapshots. */
#ifndef DKR_NVN_COMMANDS_H
#define DKR_NVN_COMMANDS_H
#include "dkr/rt.h"

typedef struct NvnPayload NvnPayload;
typedef struct NvnRecord {
    const char *name;
    uint64_t x[8], v[4];
    int64_t copy_row_stride, copy_image_stride;
    NvnPayload *payload;
} NvnRecord;

void nvn_record_capture(Cpu *c, NvnRecord *record);
void nvn_record_retain(NvnRecord *record);
void nvn_record_release(NvnRecord *record);
typedef void (*NvnRecordVisitor)(const NvnRecord *record, void *user);
/* The replay consumer visits stable arguments in command order, including inlined Call/CopyCommands. */
void dkr_nvn_visit_commands(Cpu *c, const uint64_t *handles, int count, NvnRecordVisitor visit, void *user);
#endif
