#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Host link over USB-Serial/JTAG.
 * Domain events framed as newline JSON (protocol v2). See docs/host-protocol.md.
 */

esp_err_t host_link_init(void);
bool host_link_ready(void);

/**
 * RX instrumentation. `bytes` counts raw bytes pulled off stdin, `lines` counts
 * completed newline-terminated commands dispatched. If the host writes but
 * `lines` stays 0, inbound data is not reaching this task.
 *
 * `errs` / `errno` separate the two ways that can happen: a genuinely empty
 * queue (no error) versus a broken read path (errs climbing while bytes stays
 * 0 -- the signature of stdin served by an uninstalled USB-Serial/JTAG driver).
 */
uint32_t host_link_rx_bytes(void);
uint32_t host_link_rx_lines(void);
uint32_t host_link_rx_errs(void);
int host_link_rx_errno(void);

void host_link_send_hello(void);
void host_link_send_start(void);
void host_link_send_continue(void);
void host_link_send_stop(void);
void host_link_send_beat(bool accent, uint8_t beat_in_bar, uint8_t step);
void host_link_send_position(uint32_t bar, uint8_t step, uint8_t beat, uint16_t tick,
                             bool accent);
void host_link_send_note(uint8_t note, uint8_t velocity);
/** Live key feedback for host UI: index 0..7 = S1..S8, down=1/0. */
void host_link_send_key(uint8_t index, bool down);
void host_link_send_status(uint16_t bpm, bool running, uint8_t beat_in_bar, uint8_t step,
                           uint32_t bar, uint16_t tick, bool drum_mode, uint8_t volume);
void host_link_send_pattern_dump(void);
void host_link_send_ack(const char *cmd, bool ok, uint32_t rev);
void host_link_send_error(const char *cmd, const char *msg);

typedef void (*host_on_transport_fn)(bool start, bool restart);
typedef void (*host_on_bpm_fn)(uint16_t bpm);
typedef void (*host_on_swing_fn)(uint8_t swing);
typedef void (*host_on_variation_fn)(uint8_t var);
typedef void (*host_on_fill_fn)(bool held);
typedef void (*host_on_note_fn)(uint8_t note, uint8_t velocity);
typedef void (*host_on_click_fn)(bool enabled);
typedef void (*host_on_mode_fn)(bool drum_mode);
typedef void (*host_on_volume_fn)(uint8_t volume);
typedef void (*host_on_pattern_set_fn)(uint8_t bank, uint32_t rev, const uint8_t *bytes);
typedef void (*host_on_save_fn)(void);
typedef void (*host_on_ping_fn)(void);
/** Host overdub arming: when true, device ignores S7/S8/encoder transport. */
typedef void (*host_on_record_fn)(bool armed);

typedef struct {
    host_on_transport_fn on_transport;
    host_on_bpm_fn on_bpm;
    host_on_swing_fn on_swing;
    host_on_variation_fn on_variation;
    host_on_fill_fn on_fill;
    host_on_note_fn on_note;
    host_on_click_fn on_click;
    host_on_mode_fn on_mode;
    host_on_volume_fn on_volume;
    host_on_pattern_set_fn on_pattern_set;
    host_on_save_fn on_save;
    host_on_ping_fn on_ping;
    host_on_record_fn on_record;
} host_link_handlers_t;

void host_link_set_handlers(const host_link_handlers_t *handlers);
void host_link_poll_rx(void);

#ifdef __cplusplus
}
#endif
