#ifndef DKR_NVN_REPLAY_H
#define DKR_NVN_REPLAY_H
#include "dkr/rt.h"
void dkr_nvn_replay_submit(Cpu *cpu, uint64_t device, void **state, const uint64_t *handles, int count);
void dkr_nvn_replay_present(Cpu *cpu, uint64_t texture, uint64_t frame);
void dkr_nvn_replay_queue_destroy(void *state);
void dkr_nvn_replay_cleanup(void);
#endif
