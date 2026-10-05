#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t step;         /* 0..15 grid step */
    uint8_t beat_in_bar;  /* 0..3 */
    uint16_t tick;        /* 0..383 within bar */
    uint32_t bar;
    bool accent;
    bool is_quarter;      /* quarter-note boundary */
    bool is_step;         /* 16th grid boundary (playhead) */
    uint64_t sample_index;
} audio_beat_event_t;

typedef enum {
    AUDIO_MODE_METRONOME = 0,
    AUDIO_MODE_DRUM = 1,
} audio_mode_t;

/**
 * Continuous, sample-clocked I2S renderer.
 *
 * Internal transport runs at 96 PPQN derived from the I2S sample cursor.
 * Metronome edges, sequencer ticks and external pad notes share the same
 * voice mixer so sounds never hard-cut each other.
 */
esp_err_t audio_click_init(void);
esp_err_t audio_click_set_bpm(uint16_t bpm);
esp_err_t audio_click_set_metronome(bool enabled);
esp_err_t audio_click_set_mode(audio_mode_t mode);
esp_err_t audio_click_set_volume(uint8_t volume);
uint8_t audio_click_get_volume(void);

/**
 * @param running transport state
 * @param restart true = Start (reset position); false = Continue
 */
esp_err_t audio_click_set_running(bool running, bool restart);

/**
 * Monotonic count of drum voices started: sequencer steps plus pad-triggered
 * notes. The UI compares successive reads and flashes on any change, so only
 * the delta between polls is meaningful -- never the absolute value.
 */
uint32_t audio_click_drum_hit_count(void);

/** Non-blocking position / beat event for LED/host visualization. */
bool audio_click_poll_beat(audio_beat_event_t *event);

esp_err_t audio_click_play_normal(void);
esp_err_t audio_click_play_accent(void);
esp_err_t audio_click_play_note(uint8_t note, uint8_t velocity);

/** Current transport position (safe to read from main task). */
void audio_click_get_position(uint32_t *bar, uint8_t *step, uint8_t *beat, uint16_t *tick);

/** Stop transport, flush queued events, and silence all active voices. */
esp_err_t audio_click_stop(void);

#ifdef __cplusplus
}
#endif
