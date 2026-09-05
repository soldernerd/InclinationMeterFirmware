#include "drv_rn4871.h"
#include "hal_uart.h"
#include "hal_gpio.h"
#include "hal_systick.h"
#include "pin_config.h"
#include "config.h"
#include <string.h>

/* ------------------------------------------------------------------------
 * Config state machine. One step per drv_rn4871_task() call; timeouts are
 * wall-clock via hal_systick_get_ms(), so nothing here blocks. Factory-
 * fresh module assumed — no SF,1 factory-reset, just apply our settings.
 *
 *   RESET_LOW      hold ~BLE_RESET low
 *   RESET_HIGH     release, let the module boot
 *   ENTER_CMD      send "$$$", expect "CMD>"
 *   SET_NAME       send "S-,<name>", expect "AOK"
 *   SET_SERVICES   send "SS,C0"  (Device Info + UART Transparent), expect "AOK"
 *   REBOOT         send "R,1", expect "%REBOOT%"
 *   READY          data mode: pipe payload, watch %...% status tokens
 *   FAILED         gave up; retried from scratch after RETRY_BACKOFF_MS
 * --------------------------------------------------------------------- */
typedef enum {
    ST_RESET_LOW = 0,
    ST_RESET_HIGH,
    ST_ENTER_CMD,
    ST_SET_NAME,
    ST_SET_SERVICES,
    ST_REBOOT,
    ST_READY,
    ST_FAILED,
} Rn4871State;

#define RESET_LOW_MS        15U     /* ~BLE_RESET low pulse width          */
#define BOOT_MS            350U     /* module power-on -> ready            */
#define CMD_TIMEOUT_MS     400U     /* per command/response round trip    */
#define REBOOT_TIMEOUT_MS 800U      /* R,1 -> %REBOOT% and data mode       */
#define MAX_ATTEMPTS        3U      /* command retries before FAILED       */
#define RETRY_BACKOFF_MS 10000U     /* FAILED -> restart from RESET_LOW    */

#define RX_CHUNK           64U
#define TOKEN_MAX          32U      /* longest %...% status token we keep  */

static Rn4871State     s_state;
static uint32_t        s_t0;        /* entry time of the current state    */
static uint8_t         s_attempts;
static bool            s_connected;
static Rn4871RxCallback s_rx_cb;

/* %...% token accumulator (see feed_payload). */
static char            s_tok[TOKEN_MAX];
static uint8_t         s_tok_len;
static bool            s_in_tok;

/* ---------------- low-level helpers ---------------- */

static void reset_assert(bool held)
{
    /* active LOW: held == true -> drive low (in reset) */
    hal_gpio_set(BLE_RESET_PORT, BLE_RESET_PIN, !held);
}

static void send_cmd(const char *s)
{
    /* hal_uart_write() is non-blocking (DMA). Config commands are short
     * (<20 B), sent one per state and spaced by an RX round-trip plus a
     * timeout, so the previous transfer is always long done; a command
     * dropped by a busy TX would just fall into the existing retry path. */
    (void)hal_uart_write(HAL_UART_BLE, (const uint8_t *)s, (uint16_t)strlen(s));
}

/* Sliding window for rx_contains(). File-scope so a fresh handshake
 * (retry / backoff) can wipe stale bytes from a failed attempt. */
static char    s_win[TOKEN_MAX + 8];
static uint8_t s_win_len;

static void rx_scan_reset(void) { s_win_len = 0; }

/* Scan whatever's arrived on the UART for a literal substring. Consumes
 * the RX ring as it goes (fine — during config nothing else reads it). */
static bool rx_contains(const char *needle)
{
    uint8_t buf[RX_CHUNK];
    uint16_t n;
    size_t nlen = strlen(needle);

    while ((n = hal_uart_read(HAL_UART_BLE, buf, sizeof buf)) > 0U) {
        for (uint16_t i = 0; i < n; i++) {
            if (s_win_len == sizeof s_win) {
                memmove(s_win, s_win + 1, sizeof s_win - 1U);
                s_win_len--;
            }
            s_win[s_win_len++] = (char)buf[i];
            if (s_win_len >= nlen &&
                memcmp(s_win + s_win_len - nlen, needle, nlen) == 0) {
                s_win_len = 0;
                return true;
            }
        }
    }
    return false;
}

static void enter(Rn4871State st)
{
    s_state = st;
    s_t0    = hal_systick_get_ms();
}

static void fail_or_retry(void)
{
    if (++s_attempts >= MAX_ATTEMPTS) {
        enter(ST_FAILED);
    } else {
        enter(ST_RESET_LOW);
    }
}

/* ---------------- data-mode stream parsing ---------------- */

static void dispatch_token(const char *t)
{
    /* Transparent-UART link state. %STREAM_OPEN% is the one that means
     * "a central can now exchange data"; %CONNECT% alone is just the LE
     * link. %DISCONNECT% / %REBOOT% drop us back to not-connected. */
    if (strncmp(t, "STREAM_OPEN", 11) == 0 || strncmp(t, "CONNECT", 7) == 0) {
        s_connected = true;
    } else if (strncmp(t, "DISCONNECT", 10) == 0 ||
               strncmp(t, "REBOOT", 6) == 0) {
        s_connected = false;
    }
    /* %ERR...% and anything else: ignored (no debug sink yet). */
}

/* One byte of the data-mode stream. RN4871 interleaves %...% status
 * tokens with transparent payload on the same UART. A token is '%' then
 * [A-Z0-9,_-] then '%'. If the bytes after '%' don't look like that (a
 * 0x25 byte inside binary payload), the buffered bytes are flushed back
 * out as payload — combined with the frame-layer CRC in svc_api this is
 * robust enough without a status GPIO. */
static void feed_payload_byte(uint8_t b, uint8_t *out, uint16_t *out_len)
{
    if (!s_in_tok) {
        if (b == (uint8_t)'%') {
            s_in_tok  = true;
            s_tok_len = 0;
            return;
        }
        out[(*out_len)++] = b;
        return;
    }

    if (b == (uint8_t)'%') {
        s_tok[s_tok_len] = '\0';
        dispatch_token(s_tok);
        s_in_tok = false;
        return;
    }

    bool ok = (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') ||
              b == ',' || b == '_' || b == '-';
    if (ok && s_tok_len < TOKEN_MAX - 1U) {
        s_tok[s_tok_len++] = (char)b;
        return;
    }

    /* Not a token after all — emit '%' + what we buffered + this byte. */
    out[(*out_len)++] = (uint8_t)'%';
    for (uint8_t i = 0; i < s_tok_len; i++) {
        out[(*out_len)++] = (uint8_t)s_tok[i];
    }
    out[(*out_len)++] = b;
    s_in_tok = false;
}

static void pump_data_mode(void)
{
    uint8_t  in[RX_CHUNK];
    uint8_t  out[2U * RX_CHUNK + TOKEN_MAX];   /* headroom for token bail-out */
    uint16_t n;

    while ((n = hal_uart_read(HAL_UART_BLE, in, sizeof in)) > 0U) {
        uint16_t out_len = 0;
        for (uint16_t i = 0; i < n; i++) {
            feed_payload_byte(in[i], out, &out_len);
        }
        if (out_len && s_rx_cb) {
            s_rx_cb(out, out_len);
        }
    }
}

/* ---------------- public API ---------------- */

DrvStatus drv_rn4871_init(void)
{
    s_state     = ST_RESET_LOW;
    s_t0        = hal_systick_get_ms();
    s_attempts  = 0;
    s_connected = false;
    s_in_tok    = false;
    s_tok_len   = 0;
    rx_scan_reset();
    reset_assert(true);          /* start holding it in reset */
    return DRV_OK;
}

void drv_rn4871_register_rx_callback(Rn4871RxCallback cb) { s_rx_cb = cb; }

bool drv_rn4871_is_ready(void)     { return s_state == ST_READY; }
bool drv_rn4871_is_connected(void) { return s_state == ST_READY && s_connected; }

DrvStatus drv_rn4871_send(const uint8_t *data, uint16_t len)
{
    if (s_state != ST_READY || !s_connected) {
        return DRV_ERR_NOT_READY;
    }
    return hal_uart_write(HAL_UART_BLE, data, len);
}

void drv_rn4871_task(void)
{
    uint32_t now     = hal_systick_get_ms();
    uint32_t elapsed = now - s_t0;

    switch (s_state) {
    case ST_RESET_LOW:
        reset_assert(true);
        if (elapsed >= RESET_LOW_MS) {
            reset_assert(false);
            hal_uart_rx_flush(HAL_UART_BLE);
            rx_scan_reset();
            enter(ST_RESET_HIGH);
        }
        break;

    case ST_RESET_HIGH:
        if (elapsed >= BOOT_MS) {
            hal_uart_rx_flush(HAL_UART_BLE);     /* drop the power-on banner */
            send_cmd("$$$");          /* no CR — command-mode escape */
            enter(ST_ENTER_CMD);
        }
        break;

    case ST_ENTER_CMD:
        if (rx_contains("CMD>")) {
            send_cmd("S-," BLE_DEVICE_NAME "\r\n");
            enter(ST_SET_NAME);
        } else if (elapsed >= CMD_TIMEOUT_MS) {
            fail_or_retry();
        }
        break;

    case ST_SET_NAME:
        if (rx_contains("AOK")) {
            send_cmd("SS,C0\r\n");    /* Device Info + UART Transparent */
            enter(ST_SET_SERVICES);
        } else if (elapsed >= CMD_TIMEOUT_MS) {
            fail_or_retry();
        }
        break;

    case ST_SET_SERVICES:
        if (rx_contains("AOK")) {
            send_cmd("R,1\r\n");      /* reboot to apply */
            enter(ST_REBOOT);
        } else if (elapsed >= CMD_TIMEOUT_MS) {
            fail_or_retry();
        }
        break;

    case ST_REBOOT:
        if (rx_contains("%REBOOT%") || elapsed >= REBOOT_TIMEOUT_MS) {
            hal_uart_rx_flush(HAL_UART_BLE);
            s_attempts  = 0;
            s_connected = false;
            enter(ST_READY);         /* module now auto-advertises */
        }
        break;

    case ST_READY:
        pump_data_mode();
        break;

    case ST_FAILED:
        if (elapsed >= RETRY_BACKOFF_MS) {
            s_attempts = 0;
            enter(ST_RESET_LOW);
        }
        break;

    default:
        enter(ST_FAILED);
        break;
    }
}
