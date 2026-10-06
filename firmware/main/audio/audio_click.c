#include "audio_click.h"

#include "board_pins.h"
#include "board_power.h"
#include "clock.h"
#include "pattern.h"
#include "sequencer.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "audio_engine";

#define AUDIO_SAMPLE_RATE       32000
#define AUDIO_BLOCK_FRAMES      128
#define AUDIO_STEREO_SAMPLES    (AUDIO_BLOCK_FRAMES * 2)
#define CLICK_FRAMES            288
#define AUDIO_VOICE_COUNT       12
#define AUDIO_REQUEST_QUEUE_LEN 48
#define AUDIO_BEAT_QUEUE_LEN    32

extern const uint8_t s_kick_start[] asm("_binary_kick_raw_start");
extern const uint8_t s_kick_end[] asm("_binary_kick_raw_end");
extern const uint8_t s_snare_start[] asm("_binary_snare_raw_start");
extern const uint8_t s_snare_end[] asm("_binary_snare_raw_end");
extern const uint8_t s_chh_start[] asm("_binary_hihat_closed_raw_start");
extern const uint8_t s_chh_end[] asm("_binary_hihat_closed_raw_end");
extern const uint8_t s_ohh_start[] asm("_binary_hihat_open_raw_start");
extern const uint8_t s_ohh_end[] asm("_binary_hihat_open_raw_end");
extern const uint8_t s_clap_start[] asm("_binary_clap_raw_start");
extern const uint8_t s_clap_end[] asm("_binary_clap_raw_end");
extern const uint8_t s_rim_start[] asm("_binary_rim_raw_start");
extern const uint8_t s_rim_end[] asm("_binary_rim_raw_end");

typedef enum {
    VOICE_CLICK_NORMAL = 0,
    VOICE_CLICK_ACCENT,
    VOICE_KICK,
    VOICE_SNARE,
    VOICE_CHH,
    VOICE_OHH,
    VOICE_CLAP,
    VOICE_RIM,
} voice_kind_t;

/*
 * Master output gain applied after the per-voice mix and the user volume
 * scaling. Kept separate from `control.volume` (0..127) so overall loudness
 * can exceed the volume slider's 100% ceiling without changing the relative
 * balance between voices. Values above BEATBOX_VOLUME_MAX raise loudness
 * beyond the previously achievable maximum; limit_sample() still guards
 * against int16 clipping.
 */
#define AUDIO_MASTER_GAIN 200
#define AUDIO_MASTER_GAIN_UNITY 100

typedef struct {
    voice_kind_t kind;
    uint8_t velocity;
} audio_request_t;

typedef struct {
    bool active;
    voice_kind_t kind;
    uint16_t position;
    uint16_t length;
    uint8_t velocity;
    uint32_t age;
} audio_voice_t;

typedef struct {
    bool running;
    bool restart;
    bool metronome;
    bool drum_mode;
    uint8_t volume;
    uint16_t bpm;
    uint32_t transport_generation;
    uint32_t flush_generation;
} audio_control_t;

typedef struct {
    uint32_t bar;
    uint16_t tick;
    uint8_t step;
    uint8_t beat;
} audio_position_t;

static i2s_chan_handle_t s_tx;
static int16_t s_click_normal[CLICK_FRAMES];
static int16_t s_click_accent[CLICK_FRAMES];
static int16_t s_render[AUDIO_STEREO_SAMPLES];
static QueueHandle_t s_request_queue;
static QueueHandle_t s_beat_queue;
static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_position_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_control_t s_control = {
    .running = false,
    .restart = true,
    .metronome = true,
    /* Boot applies this from main.c's s_drum_mode; keep the two in sync. */
    .drum_mode = false,
    .volume = BEATBOX_VOLUME_MAX,
    .bpm = 120,
};
/* Filled only from the audio task via sequencer callback (same-tick, no queue). */
static audio_request_t s_seq_notes[BEATBOX_TRACK_COUNT];
static uint8_t s_seq_note_count;
static audio_position_t s_position;
static bool s_ready;

static float clampf(float value, float low, float high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static void synthesize_clave(int16_t *destination, int frames, float fundamental_hz, float amplitude)
{
    float noise_lp = 0.0f;
    float previous_noise_lp = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float time = (float)i / (float)AUDIO_SAMPLE_RATE;
        const float body_envelope = expf(-time * 480.0f);
        const float attack_envelope = expf(-time * 2300.0f);
        const float raw_noise =
            ((float)((i * 1103515245u + 12345u) & 0xffff) / 32768.0f - 1.0f);
        noise_lp = 0.58f * noise_lp + 0.42f * raw_noise;
        const float noise_hp = noise_lp - previous_noise_lp;
        previous_noise_lp = noise_lp;

        const float body =
            (sinf(2.0f * (float)M_PI * fundamental_hz * time) +
             0.30f * sinf(2.0f * (float)M_PI * fundamental_hz * 1.52f * time)) *
            body_envelope;
        const float attack = noise_hp * 0.72f * attack_envelope;
        const float attack_ramp = i < 4 ? (float)i / 4.0f : 1.0f;
        const int tail_start = frames - 32;
        const float tail = i < tail_start ? 1.0f : (float)(frames - 1 - i) / 31.0f;
        const float sample =
            clampf((body + attack) * amplitude * attack_ramp * tail, -0.78f, 0.78f);
        destination[i] = (int16_t)(sample * 6200.0f);
    }
}

static uint64_t tick_interval_q32(uint16_t bpm)
{
    /* samples per tick = SAMPLE_RATE * 60 / (bpm * 96) */
    return (((uint64_t)AUDIO_SAMPLE_RATE * 60ULL) << 32) /
           ((uint64_t)beatbox_clamp_bpm(bpm) * (uint64_t)BEATBOX_PPQN_INTERNAL);
}

static audio_control_t control_snapshot(void)
{
    audio_control_t snapshot;
    portENTER_CRITICAL(&s_control_lock);
    snapshot = s_control;
    portEXIT_CRITICAL(&s_control_lock);
    return snapshot;
}

static void publish_position(uint32_t bar, uint16_t tick)
{
    portENTER_CRITICAL(&s_position_lock);
    s_position.bar = bar;
    s_position.tick = tick;
    s_position.step = (uint8_t)((tick / BEATBOX_TICKS_PER_16TH) % BEATBOX_STEPS_PER_BAR);
    s_position.beat = (uint8_t)((tick / BEATBOX_TICKS_PER_QUARTER) % BEATBOX_BEATS_PER_BAR);
    portEXIT_CRITICAL(&s_position_lock);
}

static void voice_source(voice_kind_t kind, const int16_t **source, uint16_t *length)
{
    switch (kind) {
    case VOICE_CLICK_ACCENT:
        *source = s_click_accent;
        *length = CLICK_FRAMES;
        break;
    case VOICE_KICK:
        *source = (const int16_t *)s_kick_start;
        *length = (uint16_t)((s_kick_end - s_kick_start) / sizeof(int16_t));
        break;
    case VOICE_SNARE:
        *source = (const int16_t *)s_snare_start;
        *length = (uint16_t)((s_snare_end - s_snare_start) / sizeof(int16_t));
        break;
    case VOICE_CHH:
        *source = (const int16_t *)s_chh_start;
        *length = (uint16_t)((s_chh_end - s_chh_start) / sizeof(int16_t));
        break;
    case VOICE_OHH:
        *source = (const int16_t *)s_ohh_start;
        *length = (uint16_t)((s_ohh_end - s_ohh_start) / sizeof(int16_t));
        break;
    case VOICE_CLAP:
        *source = (const int16_t *)s_clap_start;
        *length = (uint16_t)((s_clap_end - s_clap_start) / sizeof(int16_t));
        break;
    case VOICE_RIM:
        *source = (const int16_t *)s_rim_start;
        *length = (uint16_t)((s_rim_end - s_rim_start) / sizeof(int16_t));
        break;
    case VOICE_CLICK_NORMAL:
    default:
        *source = s_click_normal;
        *length = CLICK_FRAMES;
        break;
    }
}

static voice_kind_t note_to_voice(uint8_t note)
{
    switch (note) {
    case BEATBOX_NOTE_KICK:
        return VOICE_KICK;
    case BEATBOX_NOTE_SNARE:
        return VOICE_SNARE;
    case BEATBOX_NOTE_CHH:
        return VOICE_CHH;
    case BEATBOX_NOTE_OHH:
        return VOICE_OHH;
    case BEATBOX_NOTE_CLAP:
        return VOICE_CLAP;
    case BEATBOX_NOTE_RIM:
        return VOICE_RIM;
    case BEATBOX_NOTE_CLICK_ACCENT:
        return VOICE_CLICK_ACCENT;
    case BEATBOX_NOTE_CLICK_NORMAL:
    default:
        return VOICE_CLICK_NORMAL;
    }
}

static audio_voice_t *allocate_voice(audio_voice_t voices[AUDIO_VOICE_COUNT], uint32_t age)
{
    for (int i = 0; i < AUDIO_VOICE_COUNT; ++i) {
        if (!voices[i].active) {
            voices[i].age = age;
            return &voices[i];
        }
    }
    audio_voice_t *oldest = &voices[0];
    for (int i = 1; i < AUDIO_VOICE_COUNT; ++i) {
        if (voices[i].age < oldest->age) {
            oldest = &voices[i];
        }
    }
    oldest->age = age;
    return oldest;
}

/**
 * Relative instrument levels (100 = unity).
 * Closed/open hats are bright and dense on this speaker; keep them well below
 * kick/snare so they glue the groove instead of masking it.
 */
static int voice_level_q7(voice_kind_t kind)
{
    switch (kind) {
    case VOICE_CHH:
        return 34;
    case VOICE_OHH:
        return 40;
    case VOICE_CLAP:
        return 88;
    case VOICE_RIM:
        return 82;
    case VOICE_CLICK_NORMAL:
        return 70;
    case VOICE_CLICK_ACCENT:
        return 76;
    case VOICE_SNARE:
        return 100;
    case VOICE_KICK:
        return 120;
    default:
        return 100;
    }
}

static void start_voice(audio_voice_t voices[AUDIO_VOICE_COUNT], voice_kind_t kind,
                        uint8_t velocity, uint32_t age)
{
    const int16_t *source = NULL;
    uint16_t length = 0;
    voice_source(kind, &source, &length);
    (void)source;

    /*
     * Drum-machine choke rules: retriggering one instrument replaces its
     * previous tail. Closed/open hats also choke each other.
     */
    for (int i = 0; i < AUDIO_VOICE_COUNT; ++i) {
        if (!voices[i].active) {
            continue;
        }
        const bool same_kind = voices[i].kind == kind;
        const bool hats =
            (kind == VOICE_CHH || kind == VOICE_OHH) &&
            (voices[i].kind == VOICE_CHH || voices[i].kind == VOICE_OHH);
        if (same_kind || hats) {
            voices[i].active = false;
        }
    }

    audio_voice_t *voice = allocate_voice(voices, age);
    voice->active = true;
    voice->kind = kind;
    voice->position = 0;
    voice->length = length;
    voice->velocity = velocity == 0 ? 100 : velocity;
}

static void clear_voices(audio_voice_t voices[AUDIO_VOICE_COUNT])
{
    memset(voices, 0, sizeof(audio_voice_t) * AUDIO_VOICE_COUNT);
}

/*
 * Soft limiter. The previous knee (13000 -> ~18000 ceiling) sat far below the
 * int16 full scale and squashed already-valid sample peaks, which is the main
 * reason board output felt quiet. The knee is now placed near full scale so
 * normal material passes through untouched and only genuine overload is
 * rounded off. Ceiling ~31000 stays just inside int16 range.
 */
#define AUDIO_LIMIT_KNEE 26000
#define AUDIO_LIMIT_SOFT_HEADROOM 9000
#define AUDIO_LIMIT_SOFT_RANGE 4000
#define AUDIO_LIMIT_CEILING (AUDIO_LIMIT_KNEE + AUDIO_LIMIT_SOFT_RANGE)

static int16_t limit_sample(int32_t sample)
{
    const int32_t sign = sample < 0 ? -1 : 1;
    int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude <= AUDIO_LIMIT_KNEE) {
        return (int16_t)sample;
    }
    const int32_t excess = magnitude - AUDIO_LIMIT_KNEE;
    magnitude = AUDIO_LIMIT_KNEE +
                (excess * AUDIO_LIMIT_SOFT_RANGE) / (excess + AUDIO_LIMIT_SOFT_HEADROOM);
    return (int16_t)(sign * magnitude);
}

static esp_err_t queue_voice(voice_kind_t kind, uint8_t velocity)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    const audio_request_t request = {.kind = kind, .velocity = velocity};
    return xQueueSend(s_request_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

/*
 * Drum-hit counter for the LED hit array.
 *
 * Written only from the audio task, so it needs no lock: the UI treats the
 * value as opaque and merely watches for a change, which makes a torn read
 * cost at most one missed flash instead of a correctness problem.
 */
static volatile uint32_t s_drum_hits;

/*
 * A "hit" is ANY instrument starting: kick, snare, both hats, clap or rim,
 * whether it came from the sequencer or from a pad. The metronome click is
 * the single exception -- it is a timekeeper, not music, so it must never
 * light the hit array.
 *
 * Enumerated explicitly rather than by enum ordering: `kind >= VOICE_KICK`
 * would silently break if anyone reordered voice_kind_t or inserted a new
 * silent voice. Adding an instrument here is a deliberate act.
 */
static bool voice_is_drum(voice_kind_t kind)
{
    switch (kind) {
    case VOICE_KICK:
    case VOICE_SNARE:
    case VOICE_CHH:
    case VOICE_OHH:
    case VOICE_CLAP:
    case VOICE_RIM:
        return true;
    case VOICE_CLICK_NORMAL:
    case VOICE_CLICK_ACCENT:
        return false;
    }
    return false;
}

static void sequencer_note_trampoline(uint8_t note, uint8_t velocity)
{
    if (s_seq_note_count >= BEATBOX_TRACK_COUNT) {
        return;
    }
    s_seq_notes[s_seq_note_count].kind = note_to_voice(note);
    s_seq_notes[s_seq_note_count].velocity = velocity == 0 ? 100 : velocity;
    s_seq_note_count++;
}

static void audio_task(void *argument)
{
    (void)argument;

    audio_voice_t voices[AUDIO_VOICE_COUNT] = {0};
    uint64_t sample_cursor = 0;
    uint64_t next_tick_q32 = 0;
    uint64_t interval_q32 = tick_interval_q32(120);
    uint32_t tick_count = 0;
    uint32_t bar = 0;
    uint32_t voice_age = 0;
    uint32_t transport_generation = 0;
    uint32_t flush_generation = 0;
    uint16_t current_bpm = 120;
    bool running = false;

    while (true) {
        const audio_control_t control = control_snapshot();

        if (control.flush_generation != flush_generation) {
            flush_generation = control.flush_generation;
            clear_voices(voices);
            xQueueReset(s_request_queue);
            xQueueReset(s_beat_queue);
        }

        if (control.transport_generation != transport_generation) {
            transport_generation = control.transport_generation;
            running = control.running;
            if (running) {
                if (control.restart) {
                    tick_count = 0;
                    bar = 0;
                }
                next_tick_q32 = sample_cursor << 32;
                publish_position(bar, (uint16_t)(tick_count % BEATBOX_TICKS_PER_BAR));
            }
        }

        if (control.bpm != current_bpm) {
            const uint64_t old_interval = interval_q32;
            const uint64_t new_interval = tick_interval_q32(control.bpm);
            if (running) {
                const uint64_t now_q32 = sample_cursor << 32;
                if (next_tick_q32 > now_q32) {
                    const long double phase_remaining =
                        (long double)(next_tick_q32 - now_q32) / (long double)old_interval;
                    next_tick_q32 =
                        now_q32 + (uint64_t)(phase_remaining * (long double)new_interval);
                }
            }
            current_bpm = control.bpm;
            interval_q32 = new_interval;
        }

        audio_request_t request;
        while (xQueueReceive(s_request_queue, &request, 0) == pdTRUE) {
            start_voice(voices, request.kind, request.velocity, voice_age++);
            if (voice_is_drum(request.kind)) {
                s_drum_hits++;
            }
        }

        for (uint32_t frame = 0; frame < AUDIO_BLOCK_FRAMES; ++frame) {
            const uint64_t frame_q32 = (sample_cursor + frame) << 32;

            if (running && frame_q32 >= next_tick_q32) {
                const uint16_t tick_in_bar =
                    (uint16_t)(tick_count % BEATBOX_TICKS_PER_BAR);
                const uint8_t step =
                    (uint8_t)((tick_in_bar / BEATBOX_TICKS_PER_16TH) % BEATBOX_STEPS_PER_BAR);
                const uint8_t beat_in_bar =
                    (uint8_t)((tick_in_bar / BEATBOX_TICKS_PER_QUARTER) % BEATBOX_BEATS_PER_BAR);
                const bool on_step = (tick_in_bar % BEATBOX_TICKS_PER_16TH) == 0;
                const bool on_quarter = (tick_in_bar % BEATBOX_TICKS_PER_QUARTER) == 0;
                const bool accent = on_quarter && beat_in_bar == 0;

                /* Independent layers: drum sequencer and metronome click can both run. */
                if (control.drum_mode) {
                    s_seq_note_count = 0;
                    sequencer_on_tick(tick_in_bar);
                    for (uint8_t i = 0; i < s_seq_note_count; ++i) {
                        start_voice(voices, s_seq_notes[i].kind, s_seq_notes[i].velocity,
                                    voice_age++);
                    }
                    /* Count every step that actually fired, so a dense step
                     * reads as one strong flash rather than several. */
                    s_drum_hits += s_seq_note_count;
                }

                if (on_quarter && control.metronome) {
                    start_voice(voices, accent ? VOICE_CLICK_ACCENT : VOICE_CLICK_NORMAL, 90,
                                voice_age++);
                }

                if (on_step || on_quarter) {
                    const audio_beat_event_t event = {
                        .step = step,
                        .beat_in_bar = beat_in_bar,
                        .tick = tick_in_bar,
                        .bar = bar,
                        .accent = accent,
                        .is_quarter = on_quarter,
                        .is_step = on_step,
                        .sample_index = sample_cursor + frame,
                    };
                    (void)xQueueSend(s_beat_queue, &event, 0);
                }

                publish_position(bar, tick_in_bar);

                tick_count++;
                if ((tick_count % BEATBOX_TICKS_PER_BAR) == 0) {
                    bar++;
                }
                next_tick_q32 += interval_q32;
            }

            int32_t mixed = 0;
            for (int voice_index = 0; voice_index < AUDIO_VOICE_COUNT; ++voice_index) {
                audio_voice_t *voice = &voices[voice_index];
                if (!voice->active) {
                    continue;
                }
                const int16_t *source = NULL;
                uint16_t length = 0;
                voice_source(voice->kind, &source, &length);
                int32_t sample = source[voice->position++];
                sample = (sample * (int32_t)voice->velocity) / 127;
                sample = (sample * voice_level_q7(voice->kind)) / 100;
                mixed += sample;
                if (voice->position >= voice->length) {
                    voice->active = false;
                }
            }

            mixed = (mixed * (int32_t)control.volume) / BEATBOX_VOLUME_MAX;
            mixed = (mixed * AUDIO_MASTER_GAIN) / AUDIO_MASTER_GAIN_UNITY;
            const int16_t output = limit_sample(mixed);
            s_render[frame * 2] = output;
            s_render[frame * 2 + 1] = output;
        }

        size_t bytes_written = 0;
        const esp_err_t error =
            i2s_channel_write(s_tx, s_render, sizeof(s_render), &bytes_written, portMAX_DELAY);
        if (error != ESP_OK || bytes_written != sizeof(s_render)) {
            ESP_LOGE(TAG, "I2S render failed: %s (%u/%u)", esp_err_to_name(error),
                     (unsigned)bytes_written, (unsigned)sizeof(s_render));
        }
        sample_cursor += AUDIO_BLOCK_FRAMES;
    }
}

esp_err_t audio_click_init(void)
{
    ESP_RETURN_ON_FALSE(board_power_peripherals_enabled(), ESP_ERR_INVALID_STATE, TAG,
                        "enable GPIO8 before speaker");

    synthesize_clave(s_click_normal, CLICK_FRAMES, 1850.0f, 0.62f);
    synthesize_clave(s_click_accent, CLICK_FRAMES, 2450.0f, 0.68f);

    ESP_RETURN_ON_ERROR(sequencer_init(sequencer_note_trampoline), TAG, "sequencer");

    s_request_queue = xQueueCreate(AUDIO_REQUEST_QUEUE_LEN, sizeof(audio_request_t));
    s_beat_queue = xQueueCreate(AUDIO_BEAT_QUEUE_LEN, sizeof(audio_beat_event_t));
    ESP_RETURN_ON_FALSE(s_request_queue != NULL && s_beat_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "audio queues");

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = AUDIO_BLOCK_FRAMES;
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx, NULL), TAG, "new I2S channel");

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = BOARD_GPIO_SPK_BCLK,
                .ws = BOARD_GPIO_SPK_WS,
                .dout = BOARD_GPIO_SPK_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &standard_config), TAG,
                        "init I2S standard mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable I2S");

    const BaseType_t created =
        xTaskCreatePinnedToCore(audio_task, "audio_render", 6144, NULL, 8, NULL, 0);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "audio render task");
    s_ready = true;

    ESP_LOGI(TAG, "sample-clock engine ready: %d Hz, 96 PPQN, %d voices", AUDIO_SAMPLE_RATE,
             AUDIO_VOICE_COUNT);
    return ESP_OK;
}

esp_err_t audio_click_set_bpm(uint16_t bpm)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.bpm = beatbox_clamp_bpm(bpm);
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

esp_err_t audio_click_set_metronome(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.metronome = enabled;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

esp_err_t audio_click_set_mode(audio_mode_t mode)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.drum_mode = mode == AUDIO_MODE_DRUM;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

esp_err_t audio_click_set_volume(uint8_t volume)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    if (volume > BEATBOX_VOLUME_MAX) {
        volume = BEATBOX_VOLUME_MAX;
    }
    portENTER_CRITICAL(&s_control_lock);
    s_control.volume = volume;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

uint8_t audio_click_get_volume(void)
{
    portENTER_CRITICAL(&s_control_lock);
    const uint8_t volume = s_control.volume;
    portEXIT_CRITICAL(&s_control_lock);
    return volume;
}

esp_err_t audio_click_set_running(bool running, bool restart)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.running = running;
    s_control.restart = restart;
    s_control.transport_generation++;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

bool audio_click_poll_beat(audio_beat_event_t *event)
{
    if (!s_ready || event == NULL) {
        return false;
    }
    return xQueueReceive(s_beat_queue, event, 0) == pdTRUE;
}

esp_err_t audio_click_play_normal(void)
{
    return queue_voice(VOICE_CLICK_NORMAL, 110);
}

esp_err_t audio_click_play_accent(void)
{
    return queue_voice(VOICE_CLICK_ACCENT, 120);
}

esp_err_t audio_click_play_note(uint8_t note, uint8_t velocity)
{
    return queue_voice(note_to_voice(note), velocity);
}

uint32_t audio_click_drum_hit_count(void)
{
    return s_drum_hits;
}

void audio_click_get_position(uint32_t *bar, uint8_t *step, uint8_t *beat, uint16_t *tick)
{
    portENTER_CRITICAL(&s_position_lock);
    if (bar) {
        *bar = s_position.bar;
    }
    if (step) {
        *step = s_position.step;
    }
    if (beat) {
        *beat = s_position.beat;
    }
    if (tick) {
        *tick = s_position.tick;
    }
    portEXIT_CRITICAL(&s_position_lock);
}

esp_err_t audio_click_stop(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not ready");
    portENTER_CRITICAL(&s_control_lock);
    s_control.running = false;
    s_control.restart = false;
    s_control.transport_generation++;
    s_control.flush_generation++;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}
