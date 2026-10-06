#include "led_status.h"

#include "board_pins.h"
#include "board_power.h"
#include "clock.h"
#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led_status";
static led_strip_handle_t s_strip;

/*
 * Strip layout. The WS2812 chain runs RIGHT-TO-LEFT on this board, so index 0
 * is the rightmost lamp and the highest index is the leftmost one. The mode
 * indicator therefore lives at the LAST index, which puts it at the far left
 * of the keyboard as seen by the player, with the four hit pixels filling the
 * remaining positions to its right.
 *
 * The mode pixel is never animated, so the current mode stays readable while
 * the hit array is flashing beside it.
 */
#define MODE_PIXEL (BOARD_WS2812_COUNT - 1)
#define HIT_FIRST_PIXEL 0
#define HIT_PIXEL_COUNT (BOARD_WS2812_COUNT - 1)
#define HIT_SPAN (HIT_PIXEL_COUNT - 1)

/*
 * Mode palette. Five states, five colours, each owning a distinct region of
 * the wheel so no two can be confused:
 *
 *   idle (stopped)        blue
 *   metronome running     yellow
 *   pattern A running     green
 *   pattern B running     purple
 *   fill engaged          red
 *
 * Yellow is chosen for the metronome because it shares no channel pattern
 * with the others: red is (255,0,0) with no green at all, while yellow keeps
 * green high; green carries no red; blue and purple sit opposite it.
 */
#define MODE_IDLE_R 0
#define MODE_IDLE_G 40
#define MODE_IDLE_B 255 /* 停止待机 */
#define MODE_METRO_R 255
#define MODE_METRO_G 210
#define MODE_METRO_B 0 /* 节拍器练习（鼓组关闭且传送带在跑） */
#define MODE_A_R 0
#define MODE_A_G 220
#define MODE_A_B 40 /* Pattern A */
#define MODE_B_R 150
#define MODE_B_G 0
#define MODE_B_B 255 /* Pattern B */
#define MODE_FILL_R 255
#define MODE_FILL_G 0
#define MODE_FILL_B 0 /* 长按加花 */

/* Idle is dimmer than every playing state, so "armed" and "running" separate. */
#define MODE_IDLE_SCALE 96
#define MODE_ACTIVE_SCALE 170

/*
 * Hit flash: warm white. Desaturated on purpose -- the four mode colours are
 * all saturated hues, so a neutral flash can never be mistaken for the mode
 * indicator. Brightness is carried entirely by `scale`.
 */
#define HIT_R 255
#define HIT_G 240
#define HIT_B 210

/*
 * Flash length. Longer than the ~20 ms render frame so the decay is actually
 * visible, shorter than a 16th note even at the 240 BPM maximum (62 ms) so
 * consecutive hits stay individually legible.
 */
#define HIT_FLASH_US 150000

/* The encoder BPM preview borrows the hit array as a single position marker. */
#define PREVIEW_R 255
#define PREVIEW_G 140
#define PREVIEW_B 0
#define PREVIEW_SCALE 110

static int64_t s_hit_us;
static uint32_t s_last_hits;
static int64_t s_tempo_preview_until_us;
static uint16_t s_preview_bpm;

static uint8_t scale_u8(uint8_t value, uint8_t scale)
{
    return (uint8_t)(((uint16_t)value * scale) / 255u);
}

static esp_err_t set_rgb(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t scale)
{
    return led_strip_set_pixel(s_strip, index, scale_u8(r, scale), scale_u8(g, scale),
                               scale_u8(b, scale));
}

esp_err_t led_status_init(void)
{
    ESP_RETURN_ON_FALSE(board_power_peripherals_enabled(), ESP_ERR_INVALID_STATE, TAG,
                        "enable GPIO8 before LEDs");

    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_GPIO_LED_DIN,
        .max_leds = BOARD_WS2812_COUNT,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG,
                        "led_strip_new_rmt_device failed");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG, "clear failed");
    ESP_LOGW(TAG, "WS2812 ready (%d pixels: 1 mode + 4 hit)", BOARD_WS2812_COUNT);
    return ESP_OK;
}

esp_err_t led_status_set_solid_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not inited");
    for (int i = 0; i < BOARD_WS2812_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, i, r, g, b), TAG, "set_pixel");
    }
    return led_strip_refresh(s_strip);
}

esp_err_t led_status_clear(void)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not inited");
    return led_strip_clear(s_strip);
}

void led_status_show_tempo(uint16_t bpm, int64_t now_us)
{
    s_preview_bpm = bpm;
    s_tempo_preview_until_us = now_us + 450000;
}

/*
 * Squared falloff. The hit appears at full brightness on the frame after it
 * is detected and then decays fast, which reads as a struck indicator rather
 * than a slow fade.
 */
static uint8_t hit_level(int64_t now_us)
{
    int64_t age = now_us - s_hit_us;
    if (age < 0) {
        age = 0;
    }
    if (age >= HIT_FLASH_US) {
        return 0;
    }
    /* 64-bit: 255 * 150000^2 does not fit in 32 bits. */
    const uint64_t total = HIT_FLASH_US;
    const uint64_t remain = (uint64_t)(HIT_FLASH_US - age);
    return (uint8_t)((255u * remain * remain) / (total * total));
}

esp_err_t led_status_update(int64_t now_us, const led_status_input_t *input)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not inited");
    ESP_RETURN_ON_FALSE(input != NULL, ESP_ERR_INVALID_ARG, TAG, "no input");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_strip), TAG, "clear");

    /* --- pixel 0: mode indicator, steady and always meaningful --- */
    uint8_t r = MODE_IDLE_R;
    uint8_t g = MODE_IDLE_G;
    uint8_t b = MODE_IDLE_B;
    uint8_t scale = MODE_IDLE_SCALE;
    if (!input->drum_mode) {
        /*
         * No pattern can sound with the drum layer off. Split the two cases so
         * they get genuinely different colours rather than one hue at two
         * brightnesses: running means the player is practising against the
         * click, stopped means the instrument is idle.
         *
         * Checked before `fill` because fill is inert in this state and a stale
         * fill flag must not light red for something that cannot sound.
         */
        if (input->running) {
            r = MODE_METRO_R;
            g = MODE_METRO_G;
            b = MODE_METRO_B;
            scale = MODE_ACTIVE_SCALE;
        } else {
            scale = MODE_IDLE_SCALE;
        }
    } else if (input->fill) {
        /* Fill wins over A/B: it is the layer actually being heard. */
        r = MODE_FILL_R;
        g = MODE_FILL_G;
        b = MODE_FILL_B;
        scale = MODE_ACTIVE_SCALE;
    } else if (input->running) {
        if (input->variation) {
            r = MODE_B_R;
            g = MODE_B_G;
            b = MODE_B_B;
        } else {
            r = MODE_A_R;
            g = MODE_A_G;
            b = MODE_A_B;
        }
        scale = MODE_ACTIVE_SCALE;
    }
    ESP_RETURN_ON_ERROR(set_rgb(MODE_PIXEL, r, g, b, scale), TAG, "mode pixel");

    /* --- pixels 1..4: hit array --- */
    if (input->drum_hits != s_last_hits) {
        s_last_hits = input->drum_hits;
        s_hit_us = now_us;
    }

    if (now_us < s_tempo_preview_until_us) {
        /* Encoder BPM preview borrows the hit array: one marker whose
         * position encodes the tempo, so the pixels stay meaningful. */
        const uint16_t span = BEATBOX_BPM_MAX - BEATBOX_BPM_MIN;
        uint16_t offset =
            s_preview_bpm > BEATBOX_BPM_MIN ? (uint16_t)(s_preview_bpm - BEATBOX_BPM_MIN) : 0;
        if (offset > span) {
            offset = span;
        }
        const uint8_t marker =
            (uint8_t)(HIT_FIRST_PIXEL + (offset * HIT_SPAN + span / 2u) / span);
        ESP_RETURN_ON_ERROR(set_rgb(marker, PREVIEW_R, PREVIEW_G, PREVIEW_B, PREVIEW_SCALE), TAG,
                            "tempo marker");
        return led_strip_refresh(s_strip);
    }

    const uint8_t level = hit_level(now_us);
    if (level > 0) {
        for (uint8_t i = 0; i < HIT_PIXEL_COUNT; ++i) {
            ESP_RETURN_ON_ERROR(
                set_rgb((uint8_t)(HIT_FIRST_PIXEL + i), HIT_R, HIT_G, HIT_B, level), TAG,
                "hit pixel");
        }
    }
    return led_strip_refresh(s_strip);
}
