#include "drv_rn4871.h"
#include "hal_uart.h"
#include "hal_gpio.h"
#include "hal_systick.h"
#include "pin_config.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

/* RN4871 driver — state-machine over a UART command/response chat with
 * the module. Two configuration paths:
 *
 *   first boot (g_device_settings.ble_configured == false):
 *     reset -> wait for "CMD>" -> $$$ -> SF,1 -> SN,name -> SA,2 -> SS,C0 ->
 *     PS,uuid -> PC,cmd_uuid,08,40 -> PC,data_uuid,10,40 -> R,1 ->
 *     wait for reboot -> $$$ -> A -> ---  -> ble_configured = true, save
 *
 *   normal boot:
 *     reset -> wait for "CMD>" -> $$$ -> A -> ---
 *
 * Both paths land in RN4871_STATE_ADVERTISING. CONNECT / DISCONNECT
 * unsolicited strings transition to/from CONNECTED. Errors set STATE_ERROR
 * and the driver stops issuing commands until reset by the user.
 *
 * This driver only tracks/reports connection state via the
 * on_connect/on_disconnect callbacks — it deliberately does NOT touch
 * g_system_state directly (Drivers_App layer; Services/svc_ble.c is the
 * one that publishes ble_connected, matching the rest of this codebase's
 * layering). */

#define RESP_BUF_SIZE   64

/* Sub-step within the configuration sequence. Shared between full and
 * abbreviated paths — the full path steps through all entries, the
 * abbreviated path skips most. */
typedef enum {
    CFG_SF1 = 0,    /* factory reset */
    CFG_SN,         /* device name */
    CFG_SA,         /* security: just-works */
    CFG_SS,         /* services: GATT server + DIS */
    CFG_PS,         /* primary service */
    CFG_PC_CMD,     /* CMD characteristic */
    CFG_PC_DATA,    /* DATA characteristic */
    CFG_R1,         /* reboot to apply */
    CFG_DONE,
} CfgStep;

static Rn4871State   s_state          = RN4871_STATE_BOOTING;
static CfgStep       s_cfg_step       = CFG_SF1;
static uint32_t      s_state_entered_ms = 0;
static uint32_t      s_step_started_ms  = 0;
static uint32_t      s_step_timeout_ms  = RN4871_CMD_TIMEOUT_MS;
static uint8_t       s_retries        = 0;

static char          s_resp_buf[RESP_BUF_SIZE];
static uint8_t       s_resp_len       = 0;

static const char   *s_expected_resp  = "AOK";
static bool          s_full_config;

static Rn4871EventCb s_on_connect         = 0;
static Rn4871EventCb s_on_disconnect      = 0;
static Rn4871EventCb s_on_config_complete = 0;
static Rn4871DataCb  s_on_data            = 0;

/* ---------------- helpers ---------------- */

static void enter_state(Rn4871State next)
{
    s_state = next;
    s_state_entered_ms = hal_systick_get_ms();
    s_resp_len = 0;
}

/* CLAUDE.md 7.6 — No Silent Failures: hal_uart_write() can return
 * DRV_ERR_NOT_READY (previous DMA TX still in flight) or DRV_ERR_INVALID.
 * A dropped command byte would otherwise look identical to "module didn't
 * respond" and burn the step's full timeout for nothing — escalate
 * straight to RN4871_STATE_ERROR instead (this driver's own equivalent of
 * "escalated to system state": every other unrecoverable condition in this
 * file already funnels through the same state, and Drivers_App has no
 * system_state access — see CLAUDE.md 8.1). */
static bool send_str(const char *s)
{
    return hal_uart_write(HAL_UART_BLE, (const uint8_t *)s, (uint16_t)strlen(s)) == DRV_OK;
}

static void start_step(const char *cmd, const char *expected, uint32_t timeout_ms)
{
    s_expected_resp   = expected;
    s_step_timeout_ms = timeout_ms;
    s_step_started_ms = hal_systick_get_ms();
    s_resp_len        = 0;
    s_retries         = 0;
    if (cmd && !send_str(cmd)) {
        enter_state(RN4871_STATE_ERROR);
    }
}

static void retry_or_fail(const char *cmd)
{
    if (s_retries == 0) {
        s_retries = 1;
        s_step_started_ms = hal_systick_get_ms();
        s_resp_len = 0;
        if (cmd && !send_str(cmd)) {
            enter_state(RN4871_STATE_ERROR);
        }
    } else {
        enter_state(RN4871_STATE_ERROR);
    }
}

static bool resp_starts_with(const char *prefix)
{
    size_t n = strlen(prefix);
    return s_resp_len >= n && memcmp(s_resp_buf, prefix, n) == 0;
}

/* Build a config command for the current step into a static buffer. */
static const char *cfg_cmd_for(CfgStep step, const char **expected_out,
                               uint32_t *timeout_out)
{
    static char buf[80];
    *expected_out = "AOK";
    *timeout_out  = RN4871_CMD_TIMEOUT_MS;
    switch (step) {
        case CFG_SF1:
            return "SF,1\r\n";
        case CFG_SN:
            snprintf(buf, sizeof buf, "SN,%s\r\n", BLE_DEVICE_NAME);
            return buf;
        case CFG_SA:
            return "SA,2\r\n";
        case CFG_SS:
            return "SS,C0\r\n";
        case CFG_PS:
            snprintf(buf, sizeof buf, "PS,%s\r\n", BLE_SERVICE_UUID);
            return buf;
        case CFG_PC_CMD:
            snprintf(buf, sizeof buf, "PC,%s,08,40\r\n", BLE_CMD_CHAR_UUID);
            return buf;
        case CFG_PC_DATA:
            snprintf(buf, sizeof buf, "PC,%s,10,40\r\n", BLE_DATA_CHAR_UUID);
            return buf;
        case CFG_R1:
            *timeout_out = RN4871_REBOOT_TIMEOUT_MS;
            return "R,1\r\n";
        case CFG_DONE:
        default:
            return 0;
    }
}

/* ---------------- public API ---------------- */

void drv_rn4871_set_on_connect(Rn4871EventCb cb)         { s_on_connect = cb; }
void drv_rn4871_set_on_disconnect(Rn4871EventCb cb)      { s_on_disconnect = cb; }
void drv_rn4871_set_on_config_complete(Rn4871EventCb cb) { s_on_config_complete = cb; }
void drv_rn4871_set_on_data(Rn4871DataCb cb)             { s_on_data = cb; }

void drv_rn4871_init(bool already_configured)
{
    /* Pulse RN4871 reset low, then release. Real timing: assert >= 100 us,
     * wait ~5 ms after release before sending anything. We just hold it
     * for a single tick window and let the BOOTING state handle the
     * post-release wait. */
    hal_gpio_set(RN4871_RST_PORT, RN4871_RST_PIN, false);
    for (volatile uint32_t i = 0; i < 80000U; ++i) { __asm__("nop"); }
    hal_gpio_set(RN4871_RST_PORT, RN4871_RST_PIN, true);

    s_full_config  = !already_configured;
    s_cfg_step     = CFG_SF1;
    s_resp_len     = 0;
    enter_state(RN4871_STATE_BOOTING);
    s_step_timeout_ms = RN4871_BOOT_TIMEOUT_MS;
    s_step_started_ms = hal_systick_get_ms();
    s_expected_resp   = "CMD>";
}

Rn4871State drv_rn4871_get_state(void)  { return s_state; }
bool drv_rn4871_is_connected(void)      { return s_state == RN4871_STATE_CONNECTED; }

DrvStatus drv_rn4871_send_notification(const uint8_t *data, uint16_t len)
{
    if (s_state != RN4871_STATE_CONNECTED) return DRV_ERR_NOT_READY;
    return hal_uart_write(HAL_UART_BLE, data, len);
}

/* ---------------- response processing ---------------- */

static void handle_unsolicited(const char *line)
{
    if (strcmp(line, "CONNECT") == 0
        || (s_state != RN4871_STATE_CONNECTED && strstr(line, "%CONN") != 0)) {
        s_state = RN4871_STATE_CONNECTED;
        if (s_on_connect) s_on_connect();
    } else if (strcmp(line, "DISCONNECT") == 0
               || strstr(line, "%DISCONN") != 0) {
        s_state = RN4871_STATE_ADVERTISING;
        if (s_on_disconnect) s_on_disconnect();
    }
    /* %STREAM_OPEN%, %REBOOT% etc. are observed but not acted on here */
}

static void on_line_complete(void)
{
    s_resp_buf[s_resp_len] = '\0';

    /* Always scan for unsolicited connection events first */
    handle_unsolicited(s_resp_buf);

    switch (s_state) {
        case RN4871_STATE_BOOTING:
            if (resp_starts_with("CMD>") || strstr(s_resp_buf, "CMD>") != 0) {
                /* Module is alive in command mode. Send $$$ to formally
                 * enter command mode (in case it's in data mode). */
                enter_state(RN4871_STATE_ENTERING_CMD);
                s_step_timeout_ms = RN4871_CMD_TIMEOUT_MS;
                s_step_started_ms = hal_systick_get_ms();
                s_expected_resp   = "CMD>";
                if (!send_str("$$$")) {
                    enter_state(RN4871_STATE_ERROR);
                }
            }
            break;

        case RN4871_STATE_ENTERING_CMD:
            if (resp_starts_with("CMD>")) {
                if (s_full_config) {
                    enter_state(RN4871_STATE_CONFIGURING);
                    s_cfg_step = CFG_SF1;
                    const char *e; uint32_t t;
                    const char *cmd = cfg_cmd_for(s_cfg_step, &e, &t);
                    start_step(cmd, e, t);
                } else {
                    /* Abbreviated path — straight to advertising */
                    s_cfg_step = CFG_DONE;
                    enter_state(RN4871_STATE_CONFIGURING);
                    start_step("A\r\n", "AOK", RN4871_CMD_TIMEOUT_MS);
                }
            }
            break;

        case RN4871_STATE_CONFIGURING:
            if (resp_starts_with(s_expected_resp)) {
                if (s_cfg_step == CFG_R1) {
                    /* Reboot in flight — wait for module to come back */
                    enter_state(RN4871_STATE_REBOOTING);
                    s_step_timeout_ms = RN4871_REBOOT_TIMEOUT_MS;
                    s_step_started_ms = hal_systick_get_ms();
                    s_expected_resp   = "REBOOT";
                } else if (s_cfg_step == CFG_DONE) {
                    /* "A" response — advertising. Exit command mode. */
                    if (!send_str("---\r\n")) {
                        enter_state(RN4871_STATE_ERROR);
                        break;
                    }
                    enter_state(RN4871_STATE_ADVERTISING);
                    if (s_full_config && s_on_config_complete) {
                        s_on_config_complete();
                    }
                } else {
                    /* Advance to next config step */
                    s_cfg_step = (CfgStep)((unsigned)s_cfg_step + 1U);
                    const char *e; uint32_t t;
                    const char *cmd = cfg_cmd_for(s_cfg_step, &e, &t);
                    if (cmd) {
                        start_step(cmd, e, t);
                    } else {
                        enter_state(RN4871_STATE_ERROR);
                    }
                }
            } else if (resp_starts_with("ERR")) {
                enter_state(RN4871_STATE_ERROR);
            }
            break;

        case RN4871_STATE_REBOOTING:
            if (strstr(s_resp_buf, "REBOOT") != 0
                || resp_starts_with("CMD>")) {
                enter_state(RN4871_STATE_RE_ENTERING_CMD);
                s_step_timeout_ms = RN4871_CMD_TIMEOUT_MS;
                s_step_started_ms = hal_systick_get_ms();
                s_expected_resp   = "CMD>";
                if (!send_str("$$$")) {
                    enter_state(RN4871_STATE_ERROR);
                }
            }
            break;

        case RN4871_STATE_RE_ENTERING_CMD:
            if (resp_starts_with("CMD>")) {
                s_cfg_step = CFG_DONE;
                enter_state(RN4871_STATE_CONFIGURING);
                start_step("A\r\n", "AOK", RN4871_CMD_TIMEOUT_MS);
            }
            break;

        default:
            break;
    }
}

/* ---------------- main task ---------------- */

void drv_rn4871_task(void)
{
    if (s_state == RN4871_STATE_ERROR) {
        return;
    }

    /* Drain everything available since last tick. */
    uint8_t b;
    while (hal_uart_read_byte(HAL_UART_BLE, &b)) {
        /* In CONNECTED state, raw bytes are BLE payload — forward to
         * svc_ble verbatim and don't touch the response-line buffer.
         *
         * KNOWN LIMITATION: this means an unsolicited "DISCONNECT"/
         * "%DISCONN%" line the module emits inline on the same UART while
         * connected is never recognized here — handle_unsolicited() is
         * only reached outside CONNECTED (see on_line_complete()). A real
         * peer disconnect is therefore never detected by this driver;
         * drv_rn4871_is_connected() stays stuck true until a manual
         * reset. This is unchanged from the original validated reference
         * implementation (old commit 1662959) this driver was ported
         * from, not a WP5 regression — distinguishing binary BLE payload
         * bytes from inline module status text in the same byte stream
         * needs protocol detail (a delimiter, or a different
         * notification mechanism) that isn't in the RN4871 datasheet and
         * would need the RN4871/71 User's Guide (DS50002466, not
         * available in this repo) to implement correctly rather than
         * guess at. Left as a documented gap, same call as the P1_6
         * low-power-mode scope-out in Config/pin_config.h. */
        if (s_state == RN4871_STATE_CONNECTED) {
            if (s_on_data) s_on_data(b);
            continue;
        }

        if (b == '\n') {
            on_line_complete();
            s_resp_len = 0;
        } else if (b != '\r' && s_resp_len < RESP_BUF_SIZE - 1U) {
            s_resp_buf[s_resp_len++] = (char)b;
        }
    }

    /* Per-state timeout handling. */
    uint32_t now = hal_systick_get_ms();
    uint32_t elapsed = now - s_step_started_ms;

    switch (s_state) {
        case RN4871_STATE_BOOTING:
            if (elapsed > RN4871_BOOT_TIMEOUT_MS) {
                enter_state(RN4871_STATE_ERROR);
            }
            break;

        case RN4871_STATE_CONFIGURING:
            if (elapsed > s_step_timeout_ms) {
                const char *e; uint32_t t;
                const char *cmd = cfg_cmd_for(s_cfg_step, &e, &t);
                retry_or_fail(cmd);
            }
            break;

        case RN4871_STATE_ENTERING_CMD:
        case RN4871_STATE_RE_ENTERING_CMD:
            if (elapsed > s_step_timeout_ms) {
                retry_or_fail("$$$");
            }
            break;

        case RN4871_STATE_REBOOTING:
            if (elapsed > RN4871_REBOOT_TIMEOUT_MS) {
                /* Module didn't come back — sometimes the REBOOT line is
                 * silently missed. Give command-mode entry a chance. */
                enter_state(RN4871_STATE_RE_ENTERING_CMD);
                s_step_started_ms = hal_systick_get_ms();
                s_step_timeout_ms = RN4871_CMD_TIMEOUT_MS;
                s_expected_resp   = "CMD>";
                if (!send_str("$$$")) {
                    enter_state(RN4871_STATE_ERROR);
                }
            }
            break;

        default:
            break;
    }
}
