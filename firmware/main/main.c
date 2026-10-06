#include "audio_click.h"
#include "board_keys.h"
#include "board_power.h"
#include "clock.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_link.h"
#include "led_status.h"
#include "pattern.h"
#include "tempo.h"

static const char *TAG = "beatbox";
static bool s_audio_ready;
static uint8_t s_last_beat_in_bar;
static uint8_t s_last_step;
static uint32_t s_last_bar;
static uint16_t s_last_tick;
static bool s_prev_keys[8];
static bool s_fill_held;
/*
 * Drum layer is OFF at power-on: the board boots as a metronome so the player
 * gets a click to practise against without Pattern A starting up underneath
 * them. The click runs on its own switch and is also on by default, so
 * power-on + Play gives a clean metronome.
 *
 * This is NOT just a volume switch: the drum layer also gates the SEQUENCER,
 * so turning it on is what makes Pattern A audible. The web UI's drum toggle
 * sends the explicit `mode` command that flips it.
 */
static bool s_drum_mode = false;
/* Host UI overdub: lock S7 / S8 / encoder press while armed. */
static bool s_record_armed;

static void send_status(void)
{
    const uint8_t volume = s_audio_ready ? audio_click_get_volume() : 100;
    host_link_send_status(tempo_get_bpm(), tempo_is_running(), s_last_beat_in_bar, s_last_step,
                          s_last_bar, s_last_tick, s_drum_mode, volume);
}

static void apply_encoder_bpm(int8_t delta)
{
    if (delta == 0) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    tempo_set_bpm((uint16_t)((int)tempo_get_bpm() + (int)delta));
    if (s_audio_ready) {
        (void)audio_click_set_bpm(tempo_get_bpm());
    }
    led_status_show_tempo(tempo_get_bpm(), now);
    send_status();
    ESP_LOGI(TAG, "BPM -> %u", tempo_get_bpm());
}

static void render_event(const audio_beat_event_t *event)
{
    s_last_beat_in_bar = event->beat_in_bar;
    s_last_step = event->step;
    s_last_bar = event->bar;
    s_last_tick = event->tick;

    /*
     * TX budget: the beat/position stream shares one CDC endpoint with RX.
     * `position` fires every 16th note; at 240 BPM that is 16 lines/sec, and
     * each `printf`+`fflush` can block the main loop once the host stops
     * draining. A blocked main loop starves `host_link_poll_rx()`, so host
     * commands (start/continue/pattern_set) pile up unread and silently die.
     *
     * Keep the host position feed but let `render_event` decide per event
     * instead of emitting both lines unconditionally on every tick.
     */
    if (event->is_quarter) {
        host_link_send_beat(event->accent, event->beat_in_bar, event->step);
    }
    if (event->is_step) {
        host_link_send_position(event->bar, event->step, event->beat_in_bar, event->tick,
                                event->accent);
    }
}

static void transport_set(bool running, bool restart, bool from_host)
{
    if (running) {
        /*
         * Do NOT early-return when the transport already looks running.
         * `tempo_is_running()` (transport layer) and the audio engine's own
         * `s_control.running` are two separate flags and can diverge -- e.g.
         * a host `{"t":"continue"}` arrives while tempo already reports
         * running, so the old `return` here skipped audio_click_set_running()
         * entirely and the engine stayed silent with `run:false` reported to
         * the host. Always push the state down to the audio engine so the two
         * flags cannot drift apart.
         */
        const bool already_running = tempo_is_running();
        tempo_set_running(true);
        if (restart) {
            s_last_beat_in_bar = 0;
            s_last_step = 0;
            s_last_bar = 0;
            s_last_tick = 0;
        }
        if (s_audio_ready) {
            (void)audio_click_set_bpm(tempo_get_bpm());
            (void)audio_click_set_running(true, restart);
        }
        ESP_LOGW(TAG, "%s @ %u BPM (was_running=%d)", restart ? "PLAY" : "CONTINUE",
                 tempo_get_bpm(), already_running ? 1 : 0);
        if (!from_host) {
            if (restart) {
                host_link_send_start();
            } else {
                host_link_send_continue();
            }
        }
        send_status();
        return;
    }

    if (!tempo_is_running()) {
        return;
    }
    tempo_set_running(false);
    if (s_audio_ready) {
        (void)audio_click_stop();
    }
    if (s_audio_ready) {
        audio_click_get_position(&s_last_bar, &s_last_step, &s_last_beat_in_bar, &s_last_tick);
    }
    ESP_LOGW(TAG, "STOP");
    if (!from_host) {
        host_link_send_stop();
    }
    send_status();
}

static void on_host_transport(bool start, bool restart)
{
    transport_set(start, restart, true);
}

static void on_host_bpm(uint16_t bpm)
{
    tempo_set_bpm(beatbox_clamp_bpm(bpm));
    if (s_audio_ready) {
        (void)audio_click_set_bpm(tempo_get_bpm());
    }
    send_status();
    ESP_LOGI(TAG, "BPM from host -> %u", tempo_get_bpm());
}

static void on_host_swing(uint8_t swing)
{
    pattern_set_swing(swing);
    send_status();
}

static void on_host_variation(uint8_t var)
{
    pattern_request_variation(var);
    send_status();
}

static void on_host_fill(bool held)
{
    s_fill_held = held;
    pattern_set_fill(held);
    send_status();
}

static void on_host_note(uint8_t note, uint8_t velocity)
{
    /*
     * Play the pad, and nothing else.
     *
     * This used to auto-enable the drum layer when it was off, on the theory
     * that "a drum note means the user wants to hear drums". That was wrong:
     * the drum layer also gates the SEQUENCER, so with the transport running a
     * single pad tap would switch playback over to Pattern A -- the pad stopped
     * behaving like a pad and started acting as a transport control.
     *
     * Mode is now changed only by an explicit `mode` command, so a performance
     * gesture can never silently reconfigure the instrument.
     */
    if (s_audio_ready) {
        (void)audio_click_play_note(note, velocity ? velocity : 127);
    }
    host_link_send_note(note, velocity ? velocity : 127);
}

static void on_host_click(bool enabled)
{
    pattern_set_click(enabled);
    if (s_audio_ready) {
        (void)audio_click_set_metronome(enabled);
    }
    send_status();
}

static void on_host_mode(bool drum_mode)
{
    /* `mode` enables the drum layer; metronome click stays an independent switch. */
    s_drum_mode = drum_mode;
    if (s_audio_ready) {
        (void)audio_click_set_mode(drum_mode ? AUDIO_MODE_DRUM : AUDIO_MODE_METRONOME);
    }
    send_status();
}

static void on_host_volume(uint8_t volume)
{
    if (s_audio_ready) {
        (void)audio_click_set_volume(volume);
    }
    send_status();
}

static void on_host_pattern_set(uint8_t bank, uint32_t rev, const uint8_t *bytes)
{
    const esp_err_t err = pattern_set_bank(bank, rev, bytes);
    if (err == ESP_OK) {
        /*
         * Store and acknowledge. Deliberately does NOT enable the drum layer:
         * writing a pattern is not a request to start playing, and with the
         * transport running the old auto-enable made a single grid edit kick
         * Pattern A into the speakers. The user's explicit `mode` choice wins.
         */
        host_link_send_ack("pattern_set", true, pattern_revision());
        send_status();
    } else {
        host_link_send_error("pattern_set", "bad_bank");
        host_link_send_ack("pattern_set", false, pattern_revision());
    }
}

static void on_host_save(void)
{
    /* MVP: acknowledge save; NVS persistence is a follow-up. */
    host_link_send_ack("save", true, pattern_revision());
}

static void on_host_ping(void)
{
    host_link_send_pattern_dump();
    send_status();
}

static void on_host_record(bool armed)
{
    s_record_armed = armed;
    if (armed && s_fill_held) {
        on_host_fill(false);
    }
}

/*
 * One Play/Stop control. Both transport buttons share this, differing in which
 * layers they bring in when STARTING:
 *
 *   encoder knob -> metronome only (drum layer off): a click to practise to
 *   S8           -> metronome + drum layer: the pattern plays as well
 *
 * Stop behaves the same for both, and the mode is asserted only on start, so
 * stopping a performance never silently reconfigures the instrument.
 */
static void transport_button(bool with_drums)
{
    if (tempo_is_running()) {
        transport_set(false, false, false);
        return;
    }
    if (with_drums) {
        if (!s_drum_mode) {
            on_host_mode(true);
        }
    } else if (s_drum_mode) {
        on_host_mode(false);
    }
    /* Resume from the saved position; Start only when already at zero. */
    const bool at_zero = s_last_bar == 0 && s_last_step == 0 && s_last_tick == 0;
    transport_set(true, at_zero, false);
}

static void handle_pads(const board_input_snapshot_t *in)
{
    /*
     * Hardware 4×2 performance map (finger-drumming / MPC style):
     *   S1 CHH  S2 OHH  S3 Clap S4 Rim
     *   S5 Kick S6 Snare S7 A/B|Fill S8 Play
     * Foundation Kick+Snare sit on the bottom row; hats/perc above.
     */
    static const uint8_t pad_notes[6] = {
        BEATBOX_NOTE_CHH, BEATBOX_NOTE_OHH, BEATBOX_NOTE_CLAP, BEATBOX_NOTE_RIM,
        BEATBOX_NOTE_KICK, BEATBOX_NOTE_SNARE,
    };
    static int64_t s7_down_us;
    static bool s7_armed;
    static bool s7_fill_from_hold;
    const int64_t now = esp_timer_get_time();
    const int64_t fill_hold_us = 280000;

    for (int i = 0; i < 8; ++i) {
        if (in->s[i] != s_prev_keys[i]) {
            host_link_send_key((uint8_t)i, in->s[i]);
        }
    }

    for (int i = 0; i < 6; ++i) {
        if (in->s[i] && !s_prev_keys[i]) {
            on_host_note(pad_notes[i], 127);
        }
    }

    if (s_record_armed) {
        /* Recording: ignore S7 A/B|Fill and clear any pending hold state. */
        if (!in->s[6] && s_prev_keys[6]) {
            if (s7_fill_from_hold && s_fill_held) {
                on_host_fill(false);
            }
            s7_armed = false;
            s7_fill_from_hold = false;
        } else if (!in->s[6]) {
            s7_armed = false;
            s7_fill_from_hold = false;
        }
    } else {
        /* S7: hold engages Fill; short tap toggles A/B. */
        if (in->s[6] && !s_prev_keys[6]) {
            s7_down_us = now;
            s7_armed = true;
            s7_fill_from_hold = false;
        }
        if (in->s[6] && s7_armed && !s7_fill_from_hold && (now - s7_down_us) >= fill_hold_us) {
            s7_fill_from_hold = true;
            if (!s_fill_held) {
                on_host_fill(true);
            }
        }
        if (!in->s[6] && s_prev_keys[6]) {
            if (s7_fill_from_hold) {
                if (s_fill_held) {
                    on_host_fill(false);
                }
            } else if (s7_armed) {
                pattern_request_variation(pattern_variation() ? 0 : 1);
                send_status();
            }
            s7_armed = false;
            s7_fill_from_hold = false;
        }
    }

    for (int i = 0; i < 8; ++i) {
        s_prev_keys[i] = in->s[i];
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(board_keys_init());
    ESP_ERROR_CHECK(board_power_enable_peripherals());
    ESP_ERROR_CHECK(pattern_init());
    ESP_ERROR_CHECK(host_link_init());

    const host_link_handlers_t handlers = {
        .on_transport = on_host_transport,
        .on_bpm = on_host_bpm,
        .on_swing = on_host_swing,
        .on_variation = on_host_variation,
        .on_fill = on_host_fill,
        .on_note = on_host_note,
        .on_click = on_host_click,
        .on_mode = on_host_mode,
        .on_volume = on_host_volume,
        .on_pattern_set = on_host_pattern_set,
        .on_save = on_host_save,
        .on_ping = on_host_ping,
        .on_record = on_host_record,
    };
    host_link_set_handlers(&handlers);

    ESP_ERROR_CHECK(led_status_init());
    const esp_err_t audio_err = audio_click_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed (%s); LEDs/keys still run", esp_err_to_name(audio_err));
    } else {
        s_audio_ready = true;
    }
    ESP_ERROR_CHECK(tempo_init(120));
    if (s_audio_ready) {
        ESP_ERROR_CHECK(audio_click_set_bpm(tempo_get_bpm()));
        ESP_ERROR_CHECK(audio_click_set_metronome(pattern_click_enabled()));
        /*
         * Apply the boot drum-mode default to the audio engine. This MUST match
         * `s_drum_mode` above -- forcing METRONOME here silently overrode the
         * drum layer at every power-on and made the pattern sequencer inaudible.
         */
        ESP_ERROR_CHECK(
            audio_click_set_mode(s_drum_mode ? AUDIO_MODE_DRUM : AUDIO_MODE_METRONOME));
    }

    ESP_ERROR_CHECK(led_status_set_solid_rgb(0, 18, 0));
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_ERROR_CHECK(led_status_clear());

    host_link_send_hello();
    host_link_send_pattern_dump();
    send_status();
    ESP_LOGW(TAG, "Ready. Pads=S1-6, S7=A/B|Fill-hold, Play=enc/S8, USB=Serial v2");
    /* ESP_LOGW, not ESP_LOGI: sdkconfig pins the default level to WARN, which
     * compiles INFO strings out of the image entirely. Boot state must be
     * observable from the serial log. */
    ESP_LOGW(TAG, "boot mode: drum=%d click=%d volume=%u", s_drum_mode ? 1 : 0,
             pattern_click_enabled() ? 1 : 0, s_audio_ready ? audio_click_get_volume() : 0);

    int64_t last_status_us = 0;
    int64_t last_hello_us = 0;
    int64_t last_led_frame_us = 0;

    while (true) {
        board_input_snapshot_t in = {0};
        ESP_ERROR_CHECK(board_keys_poll(&in));

        /*
         * RX FIRST. `handle_pads()` / `render_event()` both write to stdout,
         * and every `fflush(stdout)` can block once the CDC TX buffer fills.
         * Polling host input before any of that keeps inbound commands
         * (start / continue / pattern_set) from being starved behind our own
         * outgoing telemetry. Previously this ran *after* handle_pads(), so a
         * saturated TX path could starve RX indefinitely.
         */
        host_link_poll_rx();

        apply_encoder_bpm(in.enc_delta);

        /*
         * Two transport buttons, deliberately different:
         *   encoder knob -> metronome only
         *   S8           -> metronome + drum layer
         * Locked out while the host REC is armed.
         */
        if (!s_record_armed) {
            if (in.s8_press) {
                /* handle_pads() already mirrors S8 into the host pad matrix. */
                transport_button(true);
            } else if (in.enc_press) {
                /* The knob has no pad-matrix entry of its own; synthesise one. */
                if (!in.s[7]) {
                    host_link_send_key(7, true);
                }
                transport_button(false);
            }
        }

        handle_pads(&in);

        const int64_t now = esp_timer_get_time();

        audio_beat_event_t beat_event;
        while (s_audio_ready && audio_click_poll_beat(&beat_event)) {
            render_event(&beat_event);
        }

        if (now - last_led_frame_us >= 20000) {
            last_led_frame_us = now;
            /*
             * Pixel 0 mirrors the live mode; pixels 1-4 flash on the hit
             * counter. `fill` is read from the pattern layer rather than the
             * mirrored s_fill_held so the LED can never disagree with what the
             * sequencer is actually playing.
             */
            const led_status_input_t led_in = {
                .running = tempo_is_running(),
                .drum_mode = s_drum_mode,
                .variation = pattern_variation(),
                .fill = pattern_fill_active(),
                .bpm = tempo_get_bpm(),
                .drum_hits = s_audio_ready ? audio_click_drum_hit_count() : 0,
            };
            (void)led_status_update(now, &led_in);
        }

        if (now - last_status_us > 500000) {
            last_status_us = now;
            if (s_audio_ready && tempo_is_running()) {
                audio_click_get_position(&s_last_bar, &s_last_step, &s_last_beat_in_bar,
                                         &s_last_tick);
            }
            send_status();
            ESP_LOGW(TAG, "rx: bytes=%lu lines=%lu errs=%lu errno=%d run=%d drum=%d",
                     (unsigned long)host_link_rx_bytes(), (unsigned long)host_link_rx_lines(),
                     (unsigned long)host_link_rx_errs(), host_link_rx_errno(),
                     tempo_is_running() ? 1 : 0, s_drum_mode ? 1 : 0);
        }
        if (now - last_hello_us > 5000000) {
            last_hello_us = now;
            host_link_send_hello();
        }

        /*
         * 1 tick (~10 ms at 100 Hz) rather than 1 ms. The loop performs
         * blocking stdio on a shared CDC endpoint; spinning at 1 ms turned
         * every TX stall into an RX starvation window. 100 Hz input polling
         * is still far above human pad interaction rates, and the audio
         * engine renders on its own task so transport timing is unaffected.
         */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
