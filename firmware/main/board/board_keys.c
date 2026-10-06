#include "board_keys.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "board_keys";

static const int s_key_gpios[8] = {
    BOARD_GPIO_S1, BOARD_GPIO_S2, BOARD_GPIO_S3, BOARD_GPIO_S4,
    BOARD_GPIO_S5, BOARD_GPIO_S6, BOARD_GPIO_S7, BOARD_GPIO_S8,
};

static pcnt_unit_handle_t s_encoder_unit;
static int s_encoder_consumed_count;

/* A press must hold this long before it counts, to reject contact bounce. */
#define PRESS_DEBOUNCE_US 25000

/*
 * Debounced push button, rising-edge only.
 *
 * The encoder switch and S8 are separate transport controls with different
 * meaning -- the knob plays the metronome alone, S8 brings the drum layer in.
 * They used to be OR-ed into one signal, which made the firmware unable to
 * tell them apart. Each gets its own state machine.
 */
typedef struct {
    bool level;
    bool stable;
    int64_t change_us;
    bool edge;
} debounce_t;

static debounce_t s_enc_db;
static debounce_t s_s8_db;

/* Returns true on the single poll where the button settles into "pressed". */
static bool debounce_update(debounce_t *d, bool level, int64_t now)
{
    d->edge = false;
    if (level != d->level) {
        d->level = level;
        d->change_us = now;
    } else if ((now - d->change_us) >= PRESS_DEBOUNCE_US && level != d->stable) {
        d->stable = level;
        if (d->stable) {
            d->edge = true;
        }
    }
    return d->edge;
}

#define ENCODER_COUNTS_PER_DETENT 4

static esp_err_t encoder_pcnt_init(void)
{
    /*
     * Espressif's official EC11 PCNT topology: two channels perform 4X
     * quadrature decoding in hardware. One physical detent is exactly four
     * counts; partial/bouncing transitions never escape as product steps.
     */
    const pcnt_unit_config_t unit_config = {
        .high_limit = 32767,
        .low_limit = -32768,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_config, &s_encoder_unit), TAG, "pcnt unit");

    const pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
    };
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(s_encoder_unit, &filter_config), TAG,
                        "pcnt filter");

    const pcnt_chan_config_t channel_a_config = {
        .edge_gpio_num = BOARD_GPIO_ENC_A,
        .level_gpio_num = BOARD_GPIO_ENC_B,
    };
    const pcnt_chan_config_t channel_b_config = {
        .edge_gpio_num = BOARD_GPIO_ENC_B,
        .level_gpio_num = BOARD_GPIO_ENC_A,
    };
    pcnt_channel_handle_t channel_a = NULL;
    pcnt_channel_handle_t channel_b = NULL;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_encoder_unit, &channel_a_config, &channel_a), TAG,
                        "pcnt channel A");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(s_encoder_unit, &channel_b_config, &channel_b), TAG,
                        "pcnt channel B");

    ESP_RETURN_ON_ERROR(
        pcnt_channel_set_edge_action(channel_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE),
        TAG, "pcnt A edge");
    ESP_RETURN_ON_ERROR(
        pcnt_channel_set_level_action(channel_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
        TAG, "pcnt A level");
    ESP_RETURN_ON_ERROR(
        pcnt_channel_set_edge_action(channel_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE),
        TAG, "pcnt B edge");
    ESP_RETURN_ON_ERROR(
        pcnt_channel_set_level_action(channel_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE),
        TAG, "pcnt B level");

    ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_encoder_unit), TAG, "pcnt enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(s_encoder_unit), TAG, "pcnt clear");
    ESP_RETURN_ON_ERROR(pcnt_unit_start(s_encoder_unit), TAG, "pcnt start");
    s_encoder_consumed_count = 0;
    return ESP_OK;
}

esp_err_t board_keys_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < 8; ++i) {
        mask |= 1ULL << s_key_gpios[i];
    }
    mask |= 1ULL << BOARD_GPIO_ENC_A;
    mask |= 1ULL << BOARD_GPIO_ENC_B;
    mask |= 1ULL << BOARD_GPIO_ENC_PRESS;

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config failed");

    ESP_RETURN_ON_ERROR(encoder_pcnt_init(), TAG, "encoder PCNT init");
    s_enc_db.level = false;
    s_enc_db.stable = false;
    s_enc_db.change_us = esp_timer_get_time();
    s_enc_db.edge = false;
    s_s8_db = s_enc_db;
    ESP_LOGW(TAG, "keys + encoder ready");
    return ESP_OK;
}

esp_err_t board_keys_poll(board_input_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < 8; ++i) {
        out->s[i] = gpio_get_level(s_key_gpios[i]) == 0;
    }

    /*
     * Two INDEPENDENT transport buttons. Do not combine them: the knob starts
     * metronome-only playback while S8 brings in the drum layer, so the rest
     * of the firmware has to know which one was actually pressed.
     */
    const int64_t now = esp_timer_get_time();
    out->enc_press = debounce_update(&s_enc_db, gpio_get_level(BOARD_GPIO_ENC_PRESS) == 0, now);
    out->s8_press = debounce_update(&s_s8_db, out->s[7], now);

    out->enc_delta = 0;

    int raw_count = 0;
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(s_encoder_unit, &raw_count), TAG, "pcnt read");
    const int pending_counts = raw_count - s_encoder_consumed_count;
    const int detents = pending_counts / ENCODER_COUNTS_PER_DETENT;
    if (s_enc_db.stable) {
        /* Do not replay movement made while the knob is pressed. */
        s_encoder_consumed_count = raw_count;
    } else if (detents != 0) {
        int bounded = detents;
        if (bounded > INT8_MAX) {
            bounded = INT8_MAX;
        } else if (bounded < INT8_MIN) {
            bounded = INT8_MIN;
        }
        out->enc_delta = (int8_t)bounded;
        s_encoder_consumed_count += bounded * ENCODER_COUNTS_PER_DETENT;
    }
    return ESP_OK;
}
