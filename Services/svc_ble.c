#include "svc_ble.h"
#include "svc_api.h"
#include "drv_rn4871.h"
#include "config.h"
#include "system_state.h"
#include <string.h>

/* BLE transport for svc_api, mirroring svc_usb.c. Two differences from
 * USB:
 *   - the RN4871 Transparent UART is a raw byte stream, so inbound frames
 *     ([CMD][LEN][PAYLOAD][CRC16]) have to be reassembled from arbitrary
 *     chunks before handing to svc_api_receive();
 *   - outbound, svc_api always builds a 64-byte zero-padded frame; we
 *     trim the trailing pad and send only the real 2+LEN+2 bytes to save
 *     BLE airtime.
 * The RN4871 config/reset state machine is pumped from drv_rn4871_task()
 * inside svc_ble_task(); nothing blocks. */

#define HDR_BYTES     2U
#define CRC_BYTES     2U
#define MAX_PAYLOAD   (USB_HID_REPORT_SIZE - HDR_BYTES - CRC_BYTES)   /* 60 */

/* ---------------- RX frame reassembly ---------------- */

static uint8_t  s_frame[USB_HID_REPORT_SIZE];
static uint16_t s_have;

static void reasm_reset(void) { s_have = 0; }

static void reasm_feed(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        if (s_have < sizeof s_frame) {
            s_frame[s_have++] = data[i];
        } else {
            /* Overflowed without ever completing a frame — drop the
             * oldest byte and keep scanning for alignment. */
            memmove(s_frame, s_frame + 1, sizeof s_frame - 1U);
            s_frame[sizeof s_frame - 1U] = data[i];
        }

        for (;;) {
            if (s_have < HDR_BYTES) {
                break;
            }
            uint8_t paylen = s_frame[1];
            if (paylen > MAX_PAYLOAD) {
                /* Implausible length — we're misaligned. Drop one byte. */
                memmove(s_frame, s_frame + 1, --s_have);
                continue;
            }
            uint16_t need = (uint16_t)(HDR_BYTES + paylen + CRC_BYTES);
            if (s_have < need) {
                break;
            }
            /* svc_api_receive() re-checks length and CRC and NACKs on a
             * mismatch, so a false-aligned frame that passes the length
             * gate but fails CRC is handled there, not here. */
            svc_api_receive(API_TRANSPORT_BLE, s_frame, need);
            s_have -= need;
            if (s_have) {
                memmove(s_frame, s_frame + need, s_have);
            }
        }
    }
}

/* drv_rn4871 payload callback (data mode, %...% tokens already stripped). */
static void rx_handler(const uint8_t *data, uint16_t len)
{
    reasm_feed(data, len);
}

/* ---------------- TX ---------------- */

static void send_via_ble(const uint8_t *data, uint16_t len)
{
    /* data is svc_api's 64-byte padded frame; real length is 2+LEN+2. */
    uint16_t real = len;
    if (len >= HDR_BYTES) {
        uint16_t framed = (uint16_t)(HDR_BYTES + data[1] + CRC_BYTES);
        if (framed <= len) {
            real = framed;
        }
    }
    if (drv_rn4871_send(data, real) != DRV_OK) {
        if (g_system_state.ble_tx_dropped_count < UINT16_MAX) {
            g_system_state.ble_tx_dropped_count++;
        }
    }
}

/* ---------------- lifecycle ---------------- */

static bool s_was_connected;

void svc_ble_init(void)
{
    s_was_connected = false;
    reasm_reset();
    svc_api_register_transport(API_TRANSPORT_BLE, send_via_ble);
    drv_rn4871_register_rx_callback(rx_handler);
    (void)drv_rn4871_init();
}

bool svc_ble_is_connected(void)
{
    return drv_rn4871_is_connected();
}

void svc_ble_task(void)
{
    drv_rn4871_task();

    bool now = drv_rn4871_is_connected();
    if (now && !s_was_connected) {
        reasm_reset();
        svc_api_connected(API_TRANSPORT_BLE);
    } else if (!now && s_was_connected) {
        svc_api_disconnected(API_TRANSPORT_BLE);
    }
    s_was_connected = now;

    g_system_state.ble_connected = now;
}
