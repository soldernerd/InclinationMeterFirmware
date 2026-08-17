#include "svc_storage.h"
#include "drv_24lc256.h"
#include "math_crc.h"
#include "config.h"
#include "system_state.h"
#include "hal_systick.h"
#include <string.h>
#include <stddef.h>

/* EEPROM layout (REV B, 2026-08-17 — per-subsystem page split, see
 * config.h's "EEPROM" section for the full rationale):
 *   0x0000  Scheduler/Timing settings page
 *   0x0100  Battery settings page
 *   0x0200  TMP236 settings page
 *   0x0300  LM35 settings page
 *   0x0400  Encoder settings page
 *   0x0500  Calibration page
 *   0x0600  reserved
 *
 * Every page: magic[0..1] + version[0..1] + crc[0..1] (6-byte header,
 * CRC covers only the page's own data bytes, not the header) + that
 * page's data. Each settings page's data is a contiguous byte range
 * within DeviceSettings — see s_sections[] below and system_state.h's
 * field-grouping comment. A version mismatch or bad CRC on any one page
 * only reseeds THAT page's fields to defaults; every other page is
 * untouched. */

#define HDR_MAGIC_0       0xA5U
#define HDR_MAGIC_1       0x5AU
#define HDR_SIZE          6U

#define CALIBRATION_BASE  EEPROM_CALIBRATION_ADDR     /* 0x0500 */

/* Blocking-helper timeout: generous margin over any single I2C
 * transaction (each bounded by hal_i2c's own 100 ms HAL timeout) plus the
 * EEPROM's up-to-5 ms write cycle and the driver's 50 ms give-up poll. */
#define STORAGE_BLOCKING_TIMEOUT_MS  200U

/* How many times to retry a single failed page write before giving up on
 * the whole pending save. A stale/incomplete result is caught by the CRC
 * check on the next boot's load path and reseeded — not silently trusted. */
#define STORAGE_WRITE_MAX_RETRIES  3U

/* ---------------- settings page table ---------------- */

typedef struct {
    uint16_t eeprom_addr;
    uint16_t version;
    size_t   offset;   /* offsetof(DeviceSettings, <first field in group>) */
    size_t   size;      /* byte span through <last field in group> */
} SettingsSection;

/* offsetof(DeviceSettings, last) + sizeof(that field) - offsetof(DeviceSettings, first) */
#define SECTION_SPAN(first, last) \
    (offsetof(DeviceSettings, last) + sizeof(((DeviceSettings *)0)->last) \
     - offsetof(DeviceSettings, first))

static const SettingsSection s_sections[] = {
    { EEPROM_SCHEDULER_SETTINGS_ADDR, EEPROM_SCHEDULER_SETTINGS_VERSION,
      offsetof(DeviceSettings, task_sensors_ms),
      SECTION_SPAN(task_sensors_ms, filter_cutoff_hz_den) },
    { EEPROM_BATTERY_SETTINGS_ADDR, EEPROM_BATTERY_SETTINGS_VERSION,
      offsetof(DeviceSettings, battery_critical_mv),
      SECTION_SPAN(battery_critical_mv, vbat_scale_den) },
    { EEPROM_TMP236_SETTINGS_ADDR, EEPROM_TMP236_SETTINGS_VERSION,
      offsetof(DeviceSettings, tmp236_seg1_voffs_mv),
      SECTION_SPAN(tmp236_seg1_voffs_mv, tmp236_seg2_tinfl_cdeg) },
    { EEPROM_LM35_SETTINGS_ADDR, EEPROM_LM35_SETTINGS_VERSION,
      offsetof(DeviceSettings, lm35_scale_mv_per_c),
      SECTION_SPAN(lm35_scale_mv_per_c, lm35_scale_mv_per_c) },
    { EEPROM_ENCODER_SETTINGS_ADDR, EEPROM_ENCODER_SETTINGS_VERSION,
      offsetof(DeviceSettings, encoder_counts_per_detent),
      SECTION_SPAN(encoder_counts_per_detent, encoder_counts_per_detent) },
};
#define SETTINGS_SECTION_COUNT  ((uint8_t)(sizeof(s_sections) / sizeof(s_sections[0])))

/* Pending-write state machine. Sized for one section's header+data (all
 * DeviceSettings sections are small, well under sizeof(DeviceSettings))
 * or a whole CalibrationData save, whichever is larger. */
typedef struct {
    bool        active;
    bool        is_settings_save;      /* true: advance through s_sections[] as each completes */
    uint8_t     section_idx;           /* current index into s_sections[], if is_settings_save */
    const DeviceSettings *settings_src; /* only valid while is_settings_save && active; must
                                          * stay alive for the whole multi-tick operation — the
                                          * only caller passes the persistent &g_device_settings */
    uint16_t    base_addr;      /* EEPROM address of the current page's header */
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

    /* Scheduler/Timing page */
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

    /* Battery page */
    s->battery_critical_mv      = DEFAULT_BATTERY_CRITICAL_MV;
    s->battery_low_mv           = DEFAULT_BATTERY_LOW_MV;
    s->vbat_scale_num           = DEFAULT_VBAT_SCALE_NUM;
    s->vbat_scale_den           = DEFAULT_VBAT_SCALE_DEN;

    /* TMP236 page */
    s->tmp236_seg1_voffs_mv     = DEFAULT_TMP236_SEG1_VOFFS_MV;
    s->tmp236_seg1_num          = DEFAULT_TMP236_SEG1_NUM;
    s->tmp236_seg1_den          = DEFAULT_TMP236_SEG1_DEN;
    s->tmp236_seg_boundary_mv   = DEFAULT_TMP236_SEG_BOUNDARY_MV;
    s->tmp236_seg2_voffs_mv     = DEFAULT_TMP236_SEG2_VOFFS_MV;
    s->tmp236_seg2_num          = DEFAULT_TMP236_SEG2_NUM;
    s->tmp236_seg2_den          = DEFAULT_TMP236_SEG2_DEN;
    s->tmp236_seg2_tinfl_cdeg   = DEFAULT_TMP236_SEG2_TINFL_CDEG;

    /* LM35 page */
    s->lm35_scale_mv_per_c      = DEFAULT_LM35_SCALE_MV_PER_C;

    /* Encoder page */
    s->encoder_counts_per_detent = DEFAULT_ENCODER_COUNTS_PER_DETENT;
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
        if (hal_systick_elapsed_ms(start_ms) > STORAGE_BLOCKING_TIMEOUT_MS) {
            /* Give up — abort rather than leaving the DMA target armed at
             * `buf`, which may be a caller's stack buffer that's about to
             * go out of scope. Also resets the driver's own state machine
             * so a stuck bus doesn't wedge every future EEPROM access. */
            drv_24lc256_abort();
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
        if (hal_systick_elapsed_ms(start_ms) > STORAGE_BLOCKING_TIMEOUT_MS) {
            drv_24lc256_abort();   /* see blocking_read()'s comment */
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

/* ---------------- settings section helpers ---------------- */

/* Loads and validates one settings page directly into *dest (which
 * points `sec->size` bytes inside a DeviceSettings). Leaves *dest
 * untouched on any failure — caller is expected to have already seeded
 * it with a default. */
static DrvStatus load_section(const SettingsSection *sec, uint8_t *dest)
{
    uint8_t hdr[HDR_SIZE];
    if (!blocking_read(sec->eeprom_addr, hdr, HDR_SIZE)) return DRV_ERR_COMM;

    uint16_t version = 0;
    uint16_t stored_crc = 0;
    if (!header_is_present(hdr, sec->version, &version, &stored_crc)) {
        return DRV_ERR_NOT_READY;       /* magic missing — first boot */
    }

    if (!blocking_read((uint16_t)(sec->eeprom_addr + HDR_SIZE), dest, (uint16_t)sec->size)) {
        return DRV_ERR_COMM;
    }

    uint16_t calc_crc = math_crc16(dest, (uint16_t)sec->size);
    if (calc_crc != stored_crc) {
        return DRV_ERR_INVALID;         /* corrupt */
    }
    if (version != sec->version) {
        return DRV_ERR_NOT_READY;       /* layout changed since this was written */
    }
    return DRV_OK;
}

/* Arms s_pending to (non-blocking, via svc_storage_update()) write one
 * settings section from `settings`. Only touches the per-section fields
 * of s_pending — active/is_settings_save/settings_src are the calling
 * operation's own bookkeeping and are left alone here. */
static void start_section_write(const DeviceSettings *settings, uint8_t idx)
{
    const SettingsSection *sec = &s_sections[idx];
    const uint8_t *field_bytes = (const uint8_t *)settings + sec->offset;

    uint16_t crc = math_crc16(field_bytes, (uint16_t)sec->size);
    build_header(s_pending.buf, sec->version, crc);
    memcpy(&s_pending.buf[HDR_SIZE], field_bytes, sec->size);

    s_pending.section_idx  = idx;
    s_pending.base_addr    = sec->eeprom_addr;
    s_pending.total_len    = (uint16_t)(HDR_SIZE + sec->size);
    s_pending.written      = 0;
    s_pending.inflight_len = 0;
    s_pending.retry_count  = 0;
}

/* ---------------- public API ---------------- */

DrvStatus svc_storage_save_settings(const DeviceSettings *settings)
{
    if (settings == 0)     return DRV_ERR_INVALID;
    if (s_pending.active)  return DRV_ERR_NOT_READY;

    s_pending.is_settings_save = true;
    s_pending.settings_src     = settings;
    s_pending.active           = true;
    start_section_write(settings, 0);
    return DRV_OK;
}

DrvStatus svc_storage_save_calibration(const CalibrationData *cal)
{
    if (cal == 0)                 return DRV_ERR_INVALID;
    if (s_pending.active)         return DRV_ERR_NOT_READY;

    uint16_t crc = math_crc16((const uint8_t *)cal, sizeof(CalibrationData));
    build_header(s_pending.buf, EEPROM_CALIBRATION_VERSION, crc);
    memcpy(&s_pending.buf[HDR_SIZE], cal, sizeof(CalibrationData));

    s_pending.is_settings_save = false;
    s_pending.base_addr    = CALIBRATION_BASE;
    s_pending.total_len    = HDR_SIZE + sizeof(CalibrationData);
    s_pending.written      = 0;
    s_pending.inflight_len = 0;
    s_pending.retry_count  = 0;
    s_pending.active       = true;
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
             * the CRC check on next boot's load and get reseeded. Also
             * abandons any remaining sections of a multi-section
             * settings save, same reasoning. */
            s_pending.active       = false;
            s_pending.inflight_len = 0U;
            s_pending.retry_count  = 0U;
            return;
        }
    }

    if (s_pending.written >= s_pending.total_len) {
        /* Current page fully written. A settings save still has more
         * pages to go — arm the next one and let the following tick
         * pick it up, rather than recursing. */
        if (s_pending.is_settings_save
            && (uint8_t)(s_pending.section_idx + 1U) < SETTINGS_SECTION_COUNT) {
            start_section_write(s_pending.settings_src, (uint8_t)(s_pending.section_idx + 1U));
            return;
        }
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

    /* Seed every field with a sane default first. A section whose page
     * turns out to be missing/corrupt/wrong-version below is simply
     * left at this default — load_section() never touches *dest on
     * failure — without disturbing any OTHER section's data. */
    fill_default_settings(&g_device_settings);

    for (uint8_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
        const SettingsSection *sec = &s_sections[i];
        uint8_t *dest = (uint8_t *)&g_device_settings + sec->offset;
        DrvStatus rc = load_section(sec, dest);
        if (rc != DRV_OK) {
            /* Persist the just-seeded default back to EEPROM so this
             * page is valid (magic/version/CRC all consistent) on the
             * next boot, matching the original single-page behavior —
             * just scoped to this one page instead of the whole struct. */
            uint8_t buf[HDR_SIZE + sizeof(DeviceSettings)];
            uint16_t crc = math_crc16(dest, (uint16_t)sec->size);
            build_header(buf, sec->version, crc);
            memcpy(&buf[HDR_SIZE], dest, sec->size);
            (void)blocking_write_block(sec->eeprom_addr, buf, (uint16_t)(HDR_SIZE + sec->size));
        }
    }

    /* Belt-and-suspenders beyond load_section()'s own guarantees: a
     * page that legitimately passed CRC+version but somehow still holds
     * a zero divisor (e.g. a future bug in whatever writes this field)
     * would otherwise fault App/app_ui.c's consume_detents(). */
    if (g_device_settings.encoder_counts_per_detent == 0U) {
        g_device_settings.encoder_counts_per_detent = DEFAULT_ENCODER_COUNTS_PER_DETENT;
    }

    /* Calibration: same pattern, but a missing/corrupt header just means
     * the device hasn't been calibrated yet — fill zeros, leave validity
     * flags false. Don't seed EEPROM with a "default" calibration; that
     * would lie about the device being calibrated. */
    DrvStatus cal_rc = svc_storage_load_calibration(&g_calibration);
    if (cal_rc != DRV_OK) {
        fill_default_calibration(&g_calibration);
    }

    g_system_state.calibration_valid =
        g_calibration.scale_valid && g_calibration.zero_valid;
}
