#include "svc_storage.h"
#include "drv_24lc256.h"
#include "math_crc.h"
#include "config.h"
#include "system_state.h"
#include "hal_systick.h"
#include <string.h>

/* EEPROM layout (per WP2 spec):
 *   0x0000  magic[0..1]                     0xA5 0x5A
 *   0x0002  settings_version[0..1]          LSB first
 *   0x0004  settings_crc[0..1]              LSB first
 *   0x0006  DeviceSettings (until 0x00FF)
 *
 *   0x0100  magic[0..1]
 *   0x0102  calibration_version[0..1]
 *   0x0104  calibration_crc[0..1]
 *   0x0106  CalibrationData (until 0x01FF)
 *
 *   0x0200  reserved
 *
 * The header (magic + version + crc) is 6 bytes; the CRC covers only the
 * struct bytes (not the header itself). */

#define HDR_MAGIC_0       0xA5U
#define HDR_MAGIC_1       0x5AU
#define HDR_SIZE          6U

#define SETTINGS_BASE     EEPROM_SETTINGS_ADDR        /* 0x0000 from config.h */
#define CALIBRATION_BASE  EEPROM_CALIBRATION_ADDR     /* 0x0100 */

/* Blocking-helper timeout: generous margin over any single I2C
 * transaction (each bounded by hal_i2c's own 100 ms HAL timeout) plus the
 * EEPROM's up-to-5 ms write cycle and the driver's 50 ms give-up poll. */
#define STORAGE_BLOCKING_TIMEOUT_MS  200U

/* How many times to retry a single failed page write before giving up on
 * the whole pending save. A stale/incomplete result is caught by the CRC
 * check on the next boot's load path and reseeded — not silently trusted. */
#define STORAGE_WRITE_MAX_RETRIES  3U

/* Pending-write state machine */
typedef struct {
    bool        active;
    uint16_t    base_addr;      /* EEPROM address of first byte (header) */
    uint8_t     buf[HDR_SIZE + sizeof(DeviceSettings) > HDR_SIZE + sizeof(CalibrationData)
                    ? HDR_SIZE + sizeof(DeviceSettings)
                    : HDR_SIZE + sizeof(CalibrationData)];
    uint16_t    total_len;
    uint16_t    written;        /* confirmed-written byte count */
    uint16_t    inflight_len;   /* length of the chunk currently in flight, 0 = none */
    uint8_t     retry_count;    /* retries attempted for the in-flight chunk */
} PendingWrite;

static PendingWrite s_pending = {0};

/* ---------------- defaults ---------------- */

static void fill_default_settings(DeviceSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->task_sensors_ms          = DEFAULT_TASK_SENSORS_MS;
    s->task_processing_ms       = DEFAULT_TASK_PROCESSING_MS;
    s->task_display_ms          = DEFAULT_TASK_DISPLAY_MS;
    s->task_ble_ms              = DEFAULT_TASK_BLE_MS;
    s->task_usb_ms              = DEFAULT_TASK_USB_MS;
    s->task_battery_ms          = DEFAULT_TASK_BATTERY_MS;
    s->task_temperature_ms      = DEFAULT_TASK_TEMPERATURE_MS;
    s->stream_interval_ms       = DEFAULT_STREAM_INTERVAL_MS;
    s->settling_threshold_umpm  = DEFAULT_SETTLING_THRESHOLD;
    s->settling_timeout_ms      = DEFAULT_SETTLING_TIMEOUT_MS;
    s->filter_cutoff_hz_num     = DEFAULT_FILTER_CUTOFF_HZ_NUM;
    s->filter_cutoff_hz_den     = DEFAULT_FILTER_CUTOFF_HZ_DEN;
    s->battery_critical_mv      = DEFAULT_BATTERY_CRITICAL_MV;
    s->battery_low_mv           = DEFAULT_BATTERY_LOW_MV;
    s->vbat_scale_num           = DEFAULT_VBAT_SCALE_NUM;
    s->vbat_scale_den           = DEFAULT_VBAT_SCALE_DEN;
    s->tmp236_seg1_voffs_mv     = DEFAULT_TMP236_SEG1_VOFFS_MV;
    s->tmp236_seg1_num          = DEFAULT_TMP236_SEG1_NUM;
    s->tmp236_seg1_den          = DEFAULT_TMP236_SEG1_DEN;
    s->tmp236_seg_boundary_mv   = DEFAULT_TMP236_SEG_BOUNDARY_MV;
    s->tmp236_seg2_voffs_mv     = DEFAULT_TMP236_SEG2_VOFFS_MV;
    s->tmp236_seg2_num          = DEFAULT_TMP236_SEG2_NUM;
    s->tmp236_seg2_den          = DEFAULT_TMP236_SEG2_DEN;
    s->tmp236_seg2_tinfl_cdeg   = DEFAULT_TMP236_SEG2_TINFL_CDEG;
    s->lm35_scale_mv_per_c      = DEFAULT_LM35_SCALE_MV_PER_C;
    s->checksum                 = 0;
}

static void fill_default_calibration(CalibrationData *c)
{
    memset(c, 0, sizeof(*c));
    c->scale_valid = false;
    c->zero_valid  = false;
}

/* ---------------- header helpers ---------------- */

/* Returns true if header magic + version match expected; CRC-checked separately */
static bool header_is_present(const uint8_t *hdr, uint16_t expected_version,
                              uint16_t *out_version, uint16_t *out_crc)
{
    if (hdr[0] != HDR_MAGIC_0 || hdr[1] != HDR_MAGIC_1) {
        return false;
    }
    *out_version = (uint16_t)(hdr[2] | (hdr[3] << 8));
    *out_crc     = (uint16_t)(hdr[4] | (hdr[5] << 8));
    (void)expected_version;
    return true;
}

static void build_header(uint8_t *hdr, uint16_t version, uint16_t crc)
{
    hdr[0] = HDR_MAGIC_0;
    hdr[1] = HDR_MAGIC_1;
    hdr[2] = (uint8_t)(version & 0xFFU);
    hdr[3] = (uint8_t)(version >> 8);
    hdr[4] = (uint8_t)(crc & 0xFFU);
    hdr[5] = (uint8_t)(crc >> 8);
}

/* Blocking read: waits for DMA to finish via the scheduler-driven update.
 * Used only at boot from svc_storage_init() before scheduler is running.
 * After firing the read, we spin on drv_24lc256_update() until done. */
static bool blocking_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (drv_24lc256_start_read(addr, buf, len) != DRV_OK) {
        return false;
    }
    /* Single-shot: poll the driver state machine ourselves */
    uint32_t start_ms = hal_systick_get_ms();
    while (drv_24lc256_is_busy()) {
        drv_24lc256_update();
        if ((uint32_t)(hal_systick_get_ms() - start_ms) > STORAGE_BLOCKING_TIMEOUT_MS) {
            return false;     /* hardware fault */
        }
    }
    return drv_24lc256_read_complete();
}

/* Blocking page write — used at boot to write defaults */
static bool blocking_write_page(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    if (drv_24lc256_start_write_page(addr, buf, len) != DRV_OK) {
        return false;
    }
    uint32_t start_ms = hal_systick_get_ms();
    while (drv_24lc256_is_busy()) {
        drv_24lc256_update();
        if ((uint32_t)(hal_systick_get_ms() - start_ms) > STORAGE_BLOCKING_TIMEOUT_MS) {
            return false;     /* hardware fault */
        }
    }
    return drv_24lc256_write_complete();
}

static bool blocking_write_block(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    uint16_t remaining = len;
    uint16_t cursor    = 0;
    while (remaining > 0) {
        uint16_t page_off  = addr & (EEPROM_PAGE_SIZE - 1U);
        uint16_t page_left = EEPROM_PAGE_SIZE - page_off;
        uint16_t chunk     = remaining < page_left ? remaining : page_left;
        if (!blocking_write_page(addr, &buf[cursor], chunk)) return false;
        addr      = (uint16_t)(addr + chunk);
        cursor    = (uint16_t)(cursor + chunk);
        remaining = (uint16_t)(remaining - chunk);
    }
    return true;
}

/* ---------------- public API ---------------- */

DrvStatus svc_storage_load_settings(DeviceSettings *settings)
{
    if (settings == 0) return DRV_ERR_INVALID;

    uint8_t hdr[HDR_SIZE];
    if (!blocking_read(SETTINGS_BASE, hdr, HDR_SIZE)) return DRV_ERR_COMM;

    uint16_t version = 0;
    uint16_t stored_crc = 0;
    if (!header_is_present(hdr, EEPROM_SETTINGS_VERSION, &version, &stored_crc)) {
        return DRV_ERR_NOT_READY;       /* magic missing — first boot */
    }

    if (!blocking_read(SETTINGS_BASE + HDR_SIZE,
                       (uint8_t *)settings, sizeof(DeviceSettings))) {
        return DRV_ERR_COMM;
    }

    uint16_t calc_crc = math_crc16((const uint8_t *)settings, sizeof(DeviceSettings));
    if (calc_crc != stored_crc) {
        return DRV_ERR_INVALID;         /* corrupt */
    }

    if (version != EEPROM_SETTINGS_VERSION) {
        /* Migration: caller can detect via version mismatch and reseed.
         * For WP2 we accept the loaded data — fields beyond what current
         * firmware understands are ignored, missing fields stay zero. */
        return DRV_ERR_NOT_READY;
    }
    return DRV_OK;
}

DrvStatus svc_storage_load_calibration(CalibrationData *cal)
{
    if (cal == 0) return DRV_ERR_INVALID;

    uint8_t hdr[HDR_SIZE];
    if (!blocking_read(CALIBRATION_BASE, hdr, HDR_SIZE)) return DRV_ERR_COMM;

    uint16_t version = 0;
    uint16_t stored_crc = 0;
    if (!header_is_present(hdr, EEPROM_CALIBRATION_VERSION, &version, &stored_crc)) {
        return DRV_ERR_NOT_READY;
    }

    if (!blocking_read(CALIBRATION_BASE + HDR_SIZE,
                       (uint8_t *)cal, sizeof(CalibrationData))) {
        return DRV_ERR_COMM;
    }

    uint16_t calc_crc = math_crc16((const uint8_t *)cal, sizeof(CalibrationData));
    if (calc_crc != stored_crc) {
        return DRV_ERR_INVALID;
    }

    if (version != EEPROM_CALIBRATION_VERSION) {
        return DRV_ERR_NOT_READY;
    }
    return DRV_OK;
}

DrvStatus svc_storage_save_settings(const DeviceSettings *settings)
{
    if (settings == 0)            return DRV_ERR_INVALID;
    if (s_pending.active)         return DRV_ERR_NOT_READY;

    uint16_t crc = math_crc16((const uint8_t *)settings, sizeof(DeviceSettings));
    build_header(s_pending.buf, EEPROM_SETTINGS_VERSION, crc);
    memcpy(&s_pending.buf[HDR_SIZE], settings, sizeof(DeviceSettings));

    s_pending.base_addr    = SETTINGS_BASE;
    s_pending.total_len    = HDR_SIZE + sizeof(DeviceSettings);
    s_pending.written      = 0;
    s_pending.inflight_len = 0;
    s_pending.retry_count  = 0;
    s_pending.active       = true;
    return DRV_OK;
}

DrvStatus svc_storage_save_calibration(const CalibrationData *cal)
{
    if (cal == 0)                 return DRV_ERR_INVALID;
    if (s_pending.active)         return DRV_ERR_NOT_READY;

    uint16_t crc = math_crc16((const uint8_t *)cal, sizeof(CalibrationData));
    build_header(s_pending.buf, EEPROM_CALIBRATION_VERSION, crc);
    memcpy(&s_pending.buf[HDR_SIZE], cal, sizeof(CalibrationData));

    s_pending.base_addr    = CALIBRATION_BASE;
    s_pending.total_len    = HDR_SIZE + sizeof(CalibrationData);
    s_pending.written      = 0;
    s_pending.inflight_len = 0;
    s_pending.retry_count  = 0;
    s_pending.active       = true;
    return DRV_OK;
}

bool svc_storage_is_busy(void)
{
    return s_pending.active;
}

void svc_storage_update(void)
{
    /* Always pump the EEPROM driver so DMA completion and write-cycle
     * polling progress every tick. */
    drv_24lc256_update();

    if (!s_pending.active) {
        return;
    }
    if (drv_24lc256_is_busy()) {
        return;     /* DMA or write-cycle poll still in flight */
    }

    if (s_pending.inflight_len != 0U) {
        /* A chunk write just finished — check the outcome before trusting
         * it and moving on. */
        if (drv_24lc256_write_complete()) {
            s_pending.written      = (uint16_t)(s_pending.written + s_pending.inflight_len);
            s_pending.inflight_len = 0U;
            s_pending.retry_count  = 0U;
        } else if (s_pending.retry_count < STORAGE_WRITE_MAX_RETRIES) {
            /* Re-issue the same chunk rather than silently treating a
             * failed write as done. */
            s_pending.retry_count++;
            (void)drv_24lc256_start_write_page(
                (uint16_t)(s_pending.base_addr + s_pending.written),
                &s_pending.buf[s_pending.written], s_pending.inflight_len);
            return;
        } else {
            /* Retries exhausted — abandon this save rather than wedge
             * forever. The header/CRC written so far (if any) will fail
             * the CRC check on next boot's load and get reseeded. */
            s_pending.active       = false;
            s_pending.inflight_len = 0U;
            s_pending.retry_count  = 0U;
            return;
        }
    }

    if (s_pending.written >= s_pending.total_len) {
        s_pending.active = false;
        return;
    }

    /* Kick off the next chunk, respecting the 64-byte page boundary */
    uint16_t addr      = (uint16_t)(s_pending.base_addr + s_pending.written);
    uint16_t remaining = (uint16_t)(s_pending.total_len - s_pending.written);
    uint16_t page_off  = addr & (EEPROM_PAGE_SIZE - 1U);
    uint16_t page_left = (uint16_t)(EEPROM_PAGE_SIZE - page_off);
    uint16_t chunk     = remaining < page_left ? remaining : page_left;
    if (drv_24lc256_start_write_page(addr, &s_pending.buf[s_pending.written], chunk) == DRV_OK) {
        s_pending.inflight_len = chunk;
    }
}

void svc_storage_init(void)
{
    drv_24lc256_init();

    /* Settings: load → write defaults if missing / corrupt / wrong version */
    DrvStatus rc = svc_storage_load_settings(&g_device_settings);
    if (rc != DRV_OK) {
        fill_default_settings(&g_device_settings);

        uint8_t hdr_and_struct[HDR_SIZE + sizeof(DeviceSettings)];
        uint16_t crc = math_crc16((const uint8_t *)&g_device_settings,
                                  sizeof(DeviceSettings));
        build_header(hdr_and_struct, EEPROM_SETTINGS_VERSION, crc);
        memcpy(&hdr_and_struct[HDR_SIZE], &g_device_settings, sizeof(DeviceSettings));
        (void)blocking_write_block(SETTINGS_BASE, hdr_and_struct, sizeof(hdr_and_struct));
    }

    /* Calibration: same pattern, but a missing/corrupt header just means
     * the device hasn't been calibrated yet — fill zeros, leave validity
     * flags false. Don't seed EEPROM with a "default" calibration; that
     * would lie about the device being calibrated. */
    rc = svc_storage_load_calibration(&g_calibration);
    if (rc != DRV_OK) {
        fill_default_calibration(&g_calibration);
    }

    g_system_state.calibration_valid =
        g_calibration.scale_valid && g_calibration.zero_valid;
}
