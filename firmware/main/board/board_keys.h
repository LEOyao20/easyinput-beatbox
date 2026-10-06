#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool s[8];
    /** Encoder knob click. Debounced, rising-edge only: Play/Stop metronome. */
    bool enc_press;
    /** S8. Debounced, rising-edge only: Play/Stop metronome + drum layer. */
    bool s8_press;
    int8_t enc_delta; /* +1 / -1 steps since last poll */
} board_input_snapshot_t;

esp_err_t board_keys_init(void);
esp_err_t board_keys_poll(board_input_snapshot_t *out);

#ifdef __cplusplus
}
#endif
