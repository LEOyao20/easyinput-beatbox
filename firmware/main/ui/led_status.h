#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_status_init(void);
esp_err_t led_status_set_solid_rgb(uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_status_clear(void);

/**
 * Everything the strip needs to render one frame.
 *
 * Layout on the 5-pixel strip:
 *   pixel 0    steady mode colour, derived from running / variation / fill
 *   pixels 1-4 hit array; all four flash together whenever `drum_hits` moves
 */
typedef struct {
    bool running;       /* transport running (false = free / idle mode) */
    uint8_t variation;  /* 0 = bank A, 1 = bank B */
    bool fill;          /* long-press fill engaged; overrides A/B */
    uint16_t bpm;       /* used only by the encoder tempo preview */
    uint32_t drum_hits; /* monotonic counter, see audio_click_drum_hit_count() */
} led_status_input_t;

/** Render one frame; call every ~20 ms. */
esp_err_t led_status_update(int64_t now_us, const led_status_input_t *input);

/** Briefly visualize the BPM range after an encoder turn. */
void led_status_show_tempo(uint16_t bpm, int64_t now_us);

#ifdef __cplusplus
}
#endif
