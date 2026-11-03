/* Host gamepad input. SDL button positions are normalized to the Switch Npad ABI. */
#ifndef DKR_GAMEPAD_H
#define DKR_GAMEPAD_H
#include <stdint.h>

typedef struct DkrGamepadState {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    int connected;
} DkrGamepadState;

void dkr_gamepad_init(void); /* Call on the main thread before starting input polling. */
void dkr_gamepad_shutdown(void);
int dkr_gamepad_poll(DkrGamepadState *state);
/* Poll both players together, preserving device ownership through hotplug. */
void dkr_gamepad_poll_players(DkrGamepadState states[2]);
void dkr_gamepad_rumble(unsigned player, float low_amp, float high_amp);
void dkr_gamepad_map(uint32_t buttons, const int16_t axes[6], unsigned deadzone, int swap_face,
                     DkrGamepadState *state);
#endif
