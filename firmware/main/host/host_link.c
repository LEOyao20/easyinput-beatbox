#include "host_link.h"

#include "clock.h"
#include "pattern.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "host";

static host_link_handlers_t s_handlers;
static bool s_ready;
static char s_rx_buf[768];
static size_t s_rx_len;
static int s_stdin_flags_set;

/* RX observability: distinguishes "host never sent" from "we never read". */
static uint32_t s_rx_bytes;
static uint32_t s_rx_lines;
/* Hard read() failures and the last errno seen -- separates "queue empty" from
 * "read path broken". A non-zero count with bytes stuck at 0 is the signature
 * of an uninstalled USJ driver serving stdin. */
static uint32_t s_rx_errs;
static int s_rx_errno;

uint32_t host_link_rx_bytes(void)
{
    return s_rx_bytes;
}

uint32_t host_link_rx_lines(void)
{
    return s_rx_lines;
}

uint32_t host_link_rx_errs(void)
{
    return s_rx_errs;
}

int host_link_rx_errno(void)
{
    return s_rx_errno;
}

static void host_write(const char *line)
{
    if (!s_ready || line == NULL) {
        return;
    }
    printf("%s\n", line);
    fflush(stdout);
}

static bool extract_quoted_type(const char *line, char *out, size_t out_len)
{
    const char *t = strstr(line, "\"t\"");
    if (t == NULL) {
        return false;
    }
    const char *colon = strchr(t, ':');
    if (colon == NULL) {
        return false;
    }
    const char *q1 = strchr(colon, '"');
    if (q1 == NULL) {
        return false;
    }
    const char *q2 = strchr(q1 + 1, '"');
    if (q2 == NULL || (size_t)(q2 - (q1 + 1)) >= out_len) {
        return false;
    }
    memcpy(out, q1 + 1, (size_t)(q2 - (q1 + 1)));
    out[q2 - (q1 + 1)] = '\0';
    return true;
}

static bool extract_int_field(const char *line, const char *key, long *out)
{
    char pattern[32];
    /* Require `"key":` so short keys like "n" never match inside "note". */
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(line, pattern);
    if (p == NULL) {
        return false;
    }
    const char *value = p + strlen(pattern);
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }
    *out = parsed;
    return true;
}

static bool extract_quoted_field(const char *line, const char *key, char *out, size_t out_len)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(line, pattern);
    if (p == NULL) {
        return false;
    }
    const char *colon = strchr(p, ':');
    if (colon == NULL) {
        return false;
    }
    const char *q1 = strchr(colon, '"');
    if (q1 == NULL) {
        return false;
    }
    const char *q2 = strchr(q1 + 1, '"');
    if (q2 == NULL || (size_t)(q2 - (q1 + 1)) >= out_len) {
        return false;
    }
    memcpy(out, q1 + 1, (size_t)(q2 - (q1 + 1)));
    out[q2 - (q1 + 1)] = '\0';
    return true;
}

esp_err_t host_link_init(void)
{
    /*
     * Install the USB-Serial/JTAG driver and route the VFS through it.
     *
     * THE RX ROOT CAUSE. With CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y the console
     * VFS attaches `usb_serial_jtag_rx_char_no_driver`, which reads the RX FIFO
     * directly. But `usb_serial_jtag_read()` sizes every read via
     * `usb_serial_jtag_get_read_bytes_available()`, and that function returns
     * `bytes_available` from the driver's RX *ringbuffer* -- which only exists
     * after `usb_serial_jtag_driver_install()`. With no driver installed it
     * unconditionally returns 0, so in non-blocking mode `fetch_size` is always
     * 0, `received` stays 0, and every `read()` returns -1/EWOULDBLOCK without
     * ever touching the FIFO.
     *
     * Symptom: host->device is dead (`rx: bytes=0` forever) while device->host
     * is perfectly healthy, because `usb_serial_jtag_write()` pushes straight
     * to the TX FIFO and never consults the ringbuffer. That asymmetry is the
     * signature of this bug.
     *
     * Installing the driver creates the RX ringbuffer the ISR fills, and
     * `usb_serial_jtag_vfs_use_driver()` swaps rx_func to `usbjtag_rx_char_via_driver`
     * so stdin drains that ringbuffer. This is exactly the pairing IDF's own
     * test_apps use.
     */
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usj_cfg.rx_buffer_size = 1024;
    usj_cfg.tx_buffer_size = 1024;
    const esp_err_t usj_err = usb_serial_jtag_driver_install(&usj_cfg);
    if (usj_err == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
    }

    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        s_stdin_flags_set = 1;
    }

    s_ready = true;
    s_rx_len = 0;
    /* ESP_LOGW: sdkconfig pins the log level to WARN, so ESP_LOGI would be
     * compiled out and this bring-up line would be invisible. */
    ESP_LOGW(TAG, "host link ready: usj_driver=%d rx=1024 tx=1024", (int)usj_err);
    host_link_send_hello();
    return ESP_OK;
}

bool host_link_ready(void)
{
    return s_ready;
}

void host_link_send_hello(void)
{
    host_write(
        "{\"t\":\"hello\",\"v\":2,\"name\":\"EasyInput Beatbox\",\"caps\":[\"drum\",\"pattern\","
        "\"swing\",\"volume\",\"record\"]}");
}

void host_link_send_start(void)
{
    host_write("{\"t\":\"start\"}");
}

void host_link_send_continue(void)
{
    host_write("{\"t\":\"continue\"}");
}

void host_link_send_stop(void)
{
    host_write("{\"t\":\"stop\"}");
}

void host_link_send_beat(bool accent, uint8_t beat_in_bar, uint8_t step)
{
    char line[128];
    snprintf(line, sizeof(line),
             "{\"t\":\"beat\",\"accent\":%d,\"beat\":%u,\"step\":%u}", accent ? 1 : 0,
             (unsigned)(beat_in_bar & 0x03), (unsigned)(step & 0x0f));
    host_write(line);
}

void host_link_send_position(uint32_t bar, uint8_t step, uint8_t beat, uint16_t tick,
                             bool accent)
{
    char line[160];
    snprintf(line, sizeof(line),
             "{\"t\":\"position\",\"bar\":%lu,\"step\":%u,\"beat\":%u,\"tick\":%u,\"accent\":%d}",
             (unsigned long)bar, (unsigned)(step & 0x0f), (unsigned)(beat & 0x03),
             (unsigned)tick, accent ? 1 : 0);
    host_write(line);
}

void host_link_send_note(uint8_t note, uint8_t velocity)
{
    char line[96];
    snprintf(line, sizeof(line), "{\"t\":\"note\",\"n\":%u,\"v\":%u}", (unsigned)note,
             (unsigned)velocity);
    host_write(line);
}

void host_link_send_key(uint8_t index, bool down)
{
    char line[72];
    snprintf(line, sizeof(line), "{\"t\":\"key\",\"i\":%u,\"v\":%d}", (unsigned)(index & 0x07),
             down ? 1 : 0);
    host_write(line);
}

void host_link_send_status(uint16_t bpm, bool running, uint8_t beat_in_bar, uint8_t step,
                           uint32_t bar, uint16_t tick, bool drum_mode, uint8_t volume)
{
    char line[288];
    snprintf(line, sizeof(line),
             "{\"t\":\"state\",\"bpm\":%u,\"run\":%d,\"beat\":%u,\"step\":%u,\"bar\":%lu,"
             "\"tick\":%u,\"swing\":%u,\"var\":%u,\"fill\":%d,\"rev\":%lu,\"click\":%d,"
             "\"mode\":%u,\"vol\":%u}",
             (unsigned)bpm, running ? 1 : 0, (unsigned)(beat_in_bar & 0x03),
             (unsigned)(step & 0x0f), (unsigned long)bar, (unsigned)tick,
             (unsigned)pattern_swing(), (unsigned)pattern_variation(),
             pattern_fill_active() ? 1 : 0, (unsigned long)pattern_revision(),
             pattern_click_enabled() ? 1 : 0, drum_mode ? 1u : 0u,
             (unsigned)(volume > BEATBOX_VOLUME_MAX ? BEATBOX_VOLUME_MAX : volume));
    host_write(line);
}

void host_link_send_pattern_dump(void)
{
    /* One bank per line — long composite dumps get truncated on USB-Serial/JTAG. */
    char hex[BEATBOX_PATTERN_HEX_CHARS + 1];
    char line[280];
    for (uint8_t bank = 0; bank < BEATBOX_BANK_COUNT; ++bank) {
        pattern_encode_hex(pattern_bank(bank), hex, sizeof(hex));
        snprintf(line, sizeof(line),
                 "{\"t\":\"pattern\",\"bank\":%u,\"rev\":%lu,\"p\":\"%s\"}", (unsigned)bank,
                 (unsigned long)pattern_revision(), hex);
        host_write(line);
    }
}

void host_link_send_ack(const char *cmd, bool ok, uint32_t rev)
{
    char line[128];
    snprintf(line, sizeof(line), "{\"t\":\"ack\",\"cmd\":\"%s\",\"ok\":%d,\"rev\":%lu}",
             cmd ? cmd : "", ok ? 1 : 0, (unsigned long)rev);
    host_write(line);
}

void host_link_send_error(const char *cmd, const char *msg)
{
    char line[192];
    snprintf(line, sizeof(line), "{\"t\":\"error\",\"cmd\":\"%s\",\"msg\":\"%s\"}",
             cmd ? cmd : "", msg ? msg : "");
    host_write(line);
}

void host_link_set_handlers(const host_link_handlers_t *handlers)
{
    if (handlers) {
        s_handlers = *handlers;
    } else {
        memset(&s_handlers, 0, sizeof(s_handlers));
    }
}

static void handle_line(const char *line)
{
    char type[32];
    if (!extract_quoted_type(line, type, sizeof(type))) {
        return;
    }

    if (strcmp(type, "start") == 0) {
        if (s_handlers.on_transport) {
            s_handlers.on_transport(true, true);
        }
        return;
    }
    if (strcmp(type, "continue") == 0) {
        if (s_handlers.on_transport) {
            s_handlers.on_transport(true, false);
        }
        return;
    }
    if (strcmp(type, "stop") == 0) {
        if (s_handlers.on_transport) {
            s_handlers.on_transport(false, false);
        }
        return;
    }
    if (strcmp(type, "ping") == 0) {
        host_link_send_hello();
        if (s_handlers.on_ping) {
            s_handlers.on_ping();
        } else {
            host_link_send_pattern_dump();
        }
        return;
    }
    if (strcmp(type, "bpm") == 0 && s_handlers.on_bpm) {
        long bpm = 120;
        if (extract_int_field(line, "v", &bpm)) {
            s_handlers.on_bpm((uint16_t)bpm);
        }
        return;
    }
    if (strcmp(type, "swing") == 0 && s_handlers.on_swing) {
        long swing = 50;
        if (extract_int_field(line, "v", &swing)) {
            s_handlers.on_swing((uint8_t)swing);
        }
        return;
    }
    if (strcmp(type, "variation") == 0 && s_handlers.on_variation) {
        long var = 0;
        if (extract_int_field(line, "v", &var)) {
            s_handlers.on_variation((uint8_t)var);
        }
        return;
    }
    if (strcmp(type, "fill") == 0 && s_handlers.on_fill) {
        long fill = 0;
        if (extract_int_field(line, "v", &fill)) {
            s_handlers.on_fill(fill != 0);
        }
        return;
    }
    if (strcmp(type, "click") == 0 && s_handlers.on_click) {
        long click = 1;
        if (extract_int_field(line, "v", &click)) {
            s_handlers.on_click(click != 0);
        }
        return;
    }
    if (strcmp(type, "mode") == 0 && s_handlers.on_mode) {
        long mode = 0;
        if (extract_int_field(line, "v", &mode)) {
            s_handlers.on_mode(mode != 0);
        }
        return;
    }
    if (strcmp(type, "volume") == 0 && s_handlers.on_volume) {
        long volume = 100;
        if (extract_int_field(line, "v", &volume)) {
            if (volume < 0) {
                volume = 0;
            }
            if (volume > BEATBOX_VOLUME_MAX) {
                volume = BEATBOX_VOLUME_MAX;
            }
            s_handlers.on_volume((uint8_t)volume);
        }
        return;
    }
    if (strcmp(type, "note") == 0 && s_handlers.on_note) {
        long n = 0;
        long v = 127;
        if (extract_int_field(line, "n", &n)) {
            (void)extract_int_field(line, "v", &v);
            s_handlers.on_note((uint8_t)n, (uint8_t)v);
        }
        return;
    }
    if (strcmp(type, "pattern_get") == 0) {
        host_link_send_pattern_dump();
        return;
    }
    if (strcmp(type, "pattern_set") == 0 && s_handlers.on_pattern_set) {
        long bank = 0;
        long rev = 0;
        char hex[BEATBOX_PATTERN_HEX_CHARS + 1];
        if (!extract_int_field(line, "bank", &bank) || !extract_int_field(line, "rev", &rev) ||
            !extract_quoted_field(line, "p", hex, sizeof(hex))) {
            host_link_send_error("pattern_set", "bad_args");
            return;
        }
        uint8_t bytes[BEATBOX_PATTERN_BYTES];
        if (pattern_decode_hex(hex, bytes) != ESP_OK) {
            host_link_send_error("pattern_set", "bad_hex");
            return;
        }
        s_handlers.on_pattern_set((uint8_t)bank, (uint32_t)rev, bytes);
        return;
    }
    if (strcmp(type, "save") == 0 && s_handlers.on_save) {
        s_handlers.on_save();
        return;
    }
    if (strcmp(type, "record") == 0 && s_handlers.on_record) {
        long armed = 0;
        if (extract_int_field(line, "v", &armed)) {
            s_handlers.on_record(armed != 0);
        }
        return;
    }

    /*
     * Anything reaching here was parsed as a valid JSON line with a "t" field
     * but matched no command — log it so a dropped command is never invisible.
     */
    ESP_LOGW(TAG, "unhandled host cmd: %s", type);
}

void host_link_poll_rx(void)
{
    if (!s_ready) {
        return;
    }

    char chunk[128];
    while (true) {
        const ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n < 0) {
            /*
             * Count hard errors separately from the benign "nothing queued"
             * case. With the USJ driver installed, an empty ringbuffer reports
             * EWOULDBLOCK/EAGAIN; anything else means the read path itself is
             * broken (e.g. the pre-driver VFS returns -1 without setting errno).
             */
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                s_rx_errno = errno ? errno : -1;
                s_rx_errs++;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        s_rx_bytes += (uint32_t)n;

        for (ssize_t i = 0; i < n; ++i) {
            const char c = chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                if (s_rx_len > 0) {
                    s_rx_buf[s_rx_len] = '\0';
                    s_rx_lines++;
                    handle_line(s_rx_buf);
                    s_rx_len = 0;
                }
                continue;
            }
            if (s_rx_len + 1 < sizeof(s_rx_buf)) {
                s_rx_buf[s_rx_len++] = c;
            } else {
                s_rx_len = 0;
            }
        }
    }

    (void)s_stdin_flags_set;
    (void)TAG;
}
