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
 *   0x0600  BLE settings page (WP5)
 *   0x0700  Displacement sensor S1 calibration page (WP10)
 *   0x0800  Displacement sensor S2 calibration page (WP10)
 *   0x0900  Displacement shared calibration page (WP10, atten)
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
    { EEPROM_BLE_SETTINGS_ADDR, EEPROM_BLE_SETTINGS_VERSION,
      offsetof(DeviceSettings, ble_configured),
      SECTION_SPAN(ble_configured, ble_configured) },
};
#define SETTINGS_SECTION_COUNT  ((uint8_t)(sizeof(s_sections) / sizeof(s_sections[0])))

/* Compile-time guarantees — code review flagged that these had only
 * been checked by a throwaway standalone build, never committed as an
 * actual guard in the source. Enforced here so a future field insertion
 * that breaks a section boundary, or a copy-pasted duplicate address,
 * fails the build instead of silently corrupting cross-page data. */
_Static_assert(offsetof(DeviceSettings, task_sensors_ms) + SECTION_SPAN(task_sensors_ms, filter_cutoff_hz_den)
                == offsetof(DeviceSettings, battery_critical_mv),
                "scheduler section must end exactly where battery section begins");
_Static_assert(offsetof(DeviceSettings, battery_critical_mv) + SECTION_SPAN(battery_critical_mv, vbat_scale_den)
                == offsetof(DeviceSettings, tmp236_seg1_voffs_mv),
                "battery section must end exactly where tmp236 section begins");
_Static_assert(offsetof(DeviceSettings, tmp236_seg1_voffs_mv) + SECTION_SPAN(tmp236_seg1_voffs_mv, tmp236_seg2_tinfl_cdeg)
                == offsetof(DeviceSettings, lm35_scale_mv_per_c),
                "tmp236 section must end exactly where lm35 section begins");
_Static_assert(offsetof(DeviceSettings, lm35_scale_mv_per_c) + SECTION_SPAN(lm35_scale_mv_per_c, lm35_scale_mv_per_c)
                == offsetof(DeviceSettings, encoder_counts_per_detent),
                "lm35 section must end exactly where encoder section begins");
_Static_assert(offsetof(DeviceSettings, encoder_counts_per_detent) + SECTION_SPAN(encoder_counts_per_detent, encoder_counts_per_detent)
                == offsetof(DeviceSettings, ble_configured),
                "encoder section must end exactly where ble section begins");
_Static_assert(offsetof(DeviceSettings, ble_configured) + SECTION_SPAN(ble_configured, ble_configured)
                == offsetof(DeviceSettings, _settings_end_marker),
                "ble section must end exactly where the struct's end-of-settings marker begins");

_Static_assert(HDR_SIZE + SECTION_SPAN(task_sensors_ms, filter_cutoff_hz_den) <= 0x0100U,
               "scheduler page must fit within its 256-byte EEPROM page budget");
_Static_assert(HDR_SIZE + SECTION_SPAN(battery_critical_mv, vbat_scale_den) <= 0x0100U,
               "battery page must fit within its 256-byte EEPROM page budget");
_Static_assert(HDR_SIZE + SECTION_SPAN(tmp236_seg1_voffs_mv, tmp236_seg2_tinfl_cdeg) <= 0x0100U,
               "tmp236 page must fit within its 256-byte EEPROM page budget");
_Static_assert(HDR_SIZE + SECTION_SPAN(lm35_scale_mv_per_c, lm35_scale_mv_per_c) <= 0x0100U,
               "lm35 page must fit within its 256-byte EEPROM page budget");
_Static_assert(HDR_SIZE + SECTION_SPAN(encoder_counts_per_detent, encoder_counts_per_detent) <= 0x0100U,
               "encoder page must fit within its 256-byte EEPROM page budget");
_Static_assert(HDR_SIZE + SECTION_SPAN(ble_configured, ble_configured) <= 0x0100U,
               "ble page must fit within its 256-byte EEPROM page budget");

/* Displacement sensor calibration pages (WP10) -- standalone structs,
 * not part of DeviceSettings/s_sections[] above (see system_state.h's
 * DisplacementSensorCal/DisplacementSharedCal comment for why), but the
 * same 256-byte-page-budget guarantee still applies. */
_Static_assert(HDR_SIZE + sizeof(DisplacementSensorCal) <= 0x0100U,
               "disp_s1/disp_s2 page must fit within its 256-byte EEPROM page budget");
_Static_assert(HDR_SIZE + sizeof(DisplacementSharedCal) <= 0x0100U,
               "disp_shared page must fit within its 256-byte EEPROM page budget");

/* Full pairwise check across all 10 page addresses — NOT just an adjacent
 * chain (A!=B && B!=C && ...). Inequality isn't transitive: a chain would
 * pass even if e.g. ENCODER and CALIBRATION silently collided, as long as
 * neither happened to equal its chain-neighbor. All C(10,2)=45 pairs: the
 * original C(7,2)=21 below, plus every one of the 3 new WP10 addresses
 * checked against all 7 prior ones and against each other (21+3=24). */
_Static_assert(EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_BATTERY_SETTINGS_ADDR
               && EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_TMP236_SETTINGS_ADDR
               && EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_LM35_SETTINGS_ADDR
               && EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_ENCODER_SETTINGS_ADDR
               && EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_BLE_SETTINGS_ADDR
               && EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_CALIBRATION_ADDR
               && EEPROM_BATTERY_SETTINGS_ADDR   != EEPROM_TMP236_SETTINGS_ADDR
               && EEPROM_BATTERY_SETTINGS_ADDR   != EEPROM_LM35_SETTINGS_ADDR
               && EEPROM_BATTERY_SETTINGS_ADDR   != EEPROM_ENCODER_SETTINGS_ADDR
               && EEPROM_BATTERY_SETTINGS_ADDR   != EEPROM_BLE_SETTINGS_ADDR
               && EEPROM_BATTERY_SETTINGS_ADDR   != EEPROM_CALIBRATION_ADDR
               && EEPROM_TMP236_SETTINGS_ADDR    != EEPROM_LM35_SETTINGS_ADDR
               && EEPROM_TMP236_SETTINGS_ADDR    != EEPROM_ENCODER_SETTINGS_ADDR
               && EEPROM_TMP236_SETTINGS_ADDR    != EEPROM_BLE_SETTINGS_ADDR
               && EEPROM_TMP236_SETTINGS_ADDR    != EEPROM_CALIBRATION_ADDR
               && EEPROM_LM35_SETTINGS_ADDR      != EEPROM_ENCODER_SETTINGS_ADDR
               && EEPROM_LM35_SETTINGS_ADDR      != EEPROM_BLE_SETTINGS_ADDR
               && EEPROM_LM35_SETTINGS_ADDR      != EEPROM_CALIBRATION_ADDR
               && EEPROM_ENCODER_SETTINGS_ADDR   != EEPROM_BLE_SETTINGS_ADDR
               && EEPROM_ENCODER_SETTINGS_ADDR   != EEPROM_CALIBRATION_ADDR
               && EEPROM_BLE_SETTINGS_ADDR       != EEPROM_CALIBRATION_ADDR
               && EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_DISP_S1_SETTINGS_ADDR
               && EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_DISP_S2_SETTINGS_ADDR
               && EEPROM_SCHEDULER_SETTINGS_ADDR != EEPROM_DISP_SHARED_SETTINGS_ADDR
               && EEPROM_BATTERY_SETTINGS_ADDR   != EEPROM_DISP_S1_SETTINGS_ADDR
               && EEPROM_BATTERY_SETTINGS_ADDR   != EEPROM_DISP_S2_SETTINGS_ADDR
               && EEPROM_BATTERY_SETTINGS_ADDR   != EEPROM_DISP_SHARED_SETTINGS_ADDR
               && EEPROM_TMP236_SETTINGS_ADDR    != EEPROM_DISP_S1_SETTINGS_ADDR
               && EEPROM_TMP236_SETTINGS_ADDR    != EEPROM_DISP_S2_SETTINGS_ADDR
               && EEPROM_TMP236_SETTINGS_ADDR    != EEPROM_DISP_SHARED_SETTINGS_ADDR
               && EEPROM_LM35_SETTINGS_ADDR      != EEPROM_DISP_S1_SETTINGS_ADDR
               && EEPROM_LM35_SETTINGS_ADDR      != EEPROM_DISP_S2_SETTINGS_ADDR
               && EEPROM_LM35_SETTINGS_ADDR      != EEPROM_DISP_SHARED_SETTINGS_ADDR
               && EEPROM_ENCODER_SETTINGS_ADDR   != EEPROM_DISP_S1_SETTINGS_ADDR
               && EEPROM_ENCODER_SETTINGS_ADDR   != EEPROM_DISP_S2_SETTINGS_ADDR
               && EEPROM_ENCODER_SETTINGS_ADDR   != EEPROM_DISP_SHARED_SETTINGS_ADDR
               && EEPROM_BLE_SETTINGS_ADDR       != EEPROM_DISP_S1_SETTINGS_ADDR
               && EEPROM_BLE_SETTINGS_ADDR       != EEPROM_DISP_S2_SETTINGS_ADDR
               && EEPROM_BLE_SETTINGS_ADDR       != EEPROM_DISP_SHARED_SETTINGS_ADDR
               && EEPROM_CALIBRATION_ADDR        != EEPROM_DISP_S1_SETTINGS_ADDR
               && EEPROM_CALIBRATION_ADDR        != EEPROM_DISP_S2_SETTINGS_ADDR
               && EEPROM_CALIBRATION_ADDR        != EEPROM_DISP_SHARED_SETTINGS_ADDR
               && EEPROM_DISP_S1_SETTINGS_ADDR   != EEPROM_DISP_S2_SETTINGS_ADDR
               && EEPROM_DISP_S1_SETTINGS_ADDR   != EEPROM_DISP_SHARED_SETTINGS_ADDR
               && EEPROM_DISP_S2_SETTINGS_ADDR   != EEPROM_DISP_SHARED_SETTINGS_ADDR,
               "every settings/calibration EEPROM page address must be distinct");

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

    /* Displacement sensor S1/S2/shared calibration (WP10) is NOT here --
     * those live in their own DisplacementSensorCal/DisplacementSharedCal
     * structs, not DeviceSettings (see system_state.h), so they're
     * defaulted where they're loaded, in svc_storage_init() below. */
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

/* Loads and validates one settings page into *dest (which points
 * `sec->size` bytes inside a DeviceSettings). Leaves *dest untouched on
 * any failure — caller is expected to have already seeded it with a
 * default. Reads into a local buffer first and only memcpy's into
 * *dest on full success (CRC AND version both matching): committing the
 * read result before validation would let corrupt/mismatched EEPROM
 * bytes overwrite a good default, and svc_storage_init()'s reseed path
 * would then compute a CRC over that corruption and persist it as
 * "valid" — the exact bug this ordering exists to prevent.
 *
 * PITFALL WORTH FLAGGING FOR ANY FUTURE EEPROM LOADER IN THIS FILE:
 * this exact write-before-validate ordering bug has already been
 * independently introduced and fixed TWICE in this codebase -- once
 * here, and once in svc_storage_load_blob() below (a WP10 code review
 * caught the second occurrence). Both fixes were per-function comments,
 * not a shared, load-bearing check, so a third loader could still get
 * this wrong. If you're adding one: validate into a local buffer first,
 * memcpy to the destination only after both CRC and version checks
 * pass -- same shape both existing loaders now use. */
static DrvStatus load_section(const SettingsSection *sec, uint8_t *dest)
{
    uint8_t hdr[HDR_SIZE];
    if (!blocking_read(sec->eeprom_addr, hdr, HDR_SIZE)) return DRV_ERR_COMM;

    uint16_t version = 0;
    uint16_t stored_crc = 0;
    if (!header_is_present(hdr, sec->version, &version, &stored_crc)) {
        return DRV_ERR_NOT_READY;       /* magic missing — first boot */
    }

    uint8_t tmp[sizeof(DeviceSettings)];   /* safe upper bound for any single section */
    if (!blocking_read((uint16_t)(sec->eeprom_addr + HDR_SIZE), tmp, (uint16_t)sec->size)) {
        return DRV_ERR_COMM;
    }

    uint16_t calc_crc = math_crc16(tmp, (uint16_t)sec->size);
    if (calc_crc != stored_crc) {
        return DRV_ERR_INVALID;         /* corrupt — dest untouched */
    }
    if (version != sec->version) {
        return DRV_ERR_NOT_READY;       /* layout changed since this was written — dest untouched */
    }

    memcpy(dest, tmp, sec->size);
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

/* Persists `data`/`len` to EEPROM at `addr` with a fresh header/CRC --
 * used when a page turns out missing/corrupt/wrong-version at boot and
 * `data` already holds a good default the caller wants written back, so
 * the page is valid (magic/version/CRC all consistent) on the next boot.
 * Shared by svc_storage_init()'s DeviceSettings section-reseed loop and
 * load_or_reseed_disp_blob() below -- previously duplicated between the
 * two (a WP10 code-review finding). */
static void reseed_blob(uint16_t addr, uint16_t version, const void *data, uint16_t len)
{
    uint8_t buf[HDR_SIZE + sizeof(DeviceSettings)];   /* largest possible reseed payload in this file */
    uint16_t crc = math_crc16((const uint8_t *)data, len);
    build_header(buf, version, crc);
    memcpy(&buf[HDR_SIZE], data, len);
    if (!blocking_write_block(addr, buf, (uint16_t)(HDR_SIZE + len))) {
        /* Not silently discarded (CLAUDE.md 7.6) -- RAM already holds the
         * correct default either way, so this boot runs correctly; only
         * the reseed-to-EEPROM step failed, meaning the same reseed will
         * be retried next boot. No DBG_PRINT infra exists yet (WP1.5 was
         * never wired up), so this reuses settings_save_failed as the
         * escalation point -- broader than just commit_edit()'s
         * user-initiated saves, but the same underlying condition (an
         * EEPROM write failed) and the same SETTINGS-screen indicator
         * applies. */
        g_system_state.settings_save_failed = true;
    }
}

/* Loads a displacement-calibration blob (WP10) into `data`/`len`, which
 * the caller has already pre-filled with defaults. svc_storage_load_blob()
 * leaves `data` untouched on any failure (same guarantee load_section()
 * gives DeviceSettings sections), so on failure this persists the
 * still-default `data` back to EEPROM via reseed_blob() -- same
 * reseed-on-failure pattern as svc_storage_init()'s DeviceSettings loop,
 * unlike g_calibration below: these DO have sane nominal defaults
 * (gain=10, d0=0.1 mm, atten=3), not "uncalibrated until proven
 * otherwise" semantics, so a missing/corrupt page gets reseeded rather
 * than left merely zeroed. */
static void load_or_reseed_disp_blob(uint16_t addr, uint16_t version,
                                     void *data, uint16_t len)
{
    if (svc_storage_load_blob(addr, version, data, len) == DRV_OK) {
        return;
    }
    reseed_blob(addr, version, data, len);
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

/* Generic single-blob save/load -- CalibrationData's original dedicated
 * pair, generalized (WP10) so the displacement-sensor calibration
 * structs (system_state.h's DisplacementSensorCal/DisplacementSharedCal,
 * three independent pages) don't need three more near-copies of the
 * same header+CRC+version bookkeeping. Any future small standalone
 * calibration struct (own page, not threaded through DeviceSettings'
 * SettingsSection mechanism) should use these directly rather than
 * hand-rolling another copy. */
DrvStatus svc_storage_save_blob(uint16_t base_addr, uint16_t version,
                                const void *data, uint16_t len)
{
    if (data == 0)                                        return DRV_ERR_INVALID;
    if ((uint32_t)HDR_SIZE + len > sizeof(s_pending.buf))  return DRV_ERR_INVALID;
    if (s_pending.active)                                 return DRV_ERR_NOT_READY;

    uint16_t crc = math_crc16((const uint8_t *)data, len);
    build_header(s_pending.buf, version, crc);
    memcpy(&s_pending.buf[HDR_SIZE], data, len);

    s_pending.is_settings_save = false;
    s_pending.base_addr    = base_addr;
    s_pending.total_len    = (uint16_t)(HDR_SIZE + len);
    s_pending.written      = 0;
    s_pending.inflight_len = 0;
    s_pending.retry_count  = 0;
    s_pending.active       = true;
    return DRV_OK;
}

DrvStatus svc_storage_load_blob(uint16_t base_addr, uint16_t version,
                                void *data, uint16_t len)
{
    if (data == 0) return DRV_ERR_INVALID;

    uint8_t hdr[HDR_SIZE];
    if (!blocking_read(base_addr, hdr, HDR_SIZE)) return DRV_ERR_COMM;

    uint16_t stored_version = 0;
    uint16_t stored_crc = 0;
    if (!header_is_present(hdr, version, &stored_version, &stored_crc)) {
        return DRV_ERR_NOT_READY;
    }

    /* Read into a local buffer first and only commit to *data on full
     * success (CRC AND version both matching) -- same reasoning as
     * load_section()'s own comment above: committing the read result
     * before validation would let corrupt/mismatched EEPROM bytes
     * overwrite a caller's already-seeded default, and a subsequent
     * reseed-on-failure path would then persist that corruption as
     * "valid" (the exact bug this ordering exists to prevent). Sized
     * for the largest blob any current caller passes (CalibrationData);
     * a future much-larger blob would need this bumped, but len is
     * checked against it here rather than silently overflowing. */
    uint8_t tmp[sizeof(CalibrationData)];
    if (len > sizeof(tmp)) {
        return DRV_ERR_INVALID;
    }
    if (!blocking_read((uint16_t)(base_addr + HDR_SIZE), tmp, len)) {
        return DRV_ERR_COMM;
    }

    uint16_t calc_crc = math_crc16(tmp, len);
    if (calc_crc != stored_crc) {
        return DRV_ERR_INVALID;         /* corrupt -- *data untouched */
    }
    if (stored_version != version) {
        return DRV_ERR_NOT_READY;       /* layout changed -- *data untouched */
    }

    memcpy(data, tmp, len);
    return DRV_OK;
}

DrvStatus svc_storage_save_calibration(const CalibrationData *cal)
{
    if (cal == 0) return DRV_ERR_INVALID;
    return svc_storage_save_blob(CALIBRATION_BASE, EEPROM_CALIBRATION_VERSION,
                                 cal, sizeof(*cal));
}

DrvStatus svc_storage_load_calibration(CalibrationData *cal)
{
    if (cal == 0) return DRV_ERR_INVALID;
    return svc_storage_load_blob(CALIBRATION_BASE, EEPROM_CALIBRATION_VERSION,
                                 cal, sizeof(*cal));
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
             * settings save, same reasoning — an aborted save can leave
             * some pages holding the new values and others the old ones
             * (not atomic across pages; the single-page design this
             * replaced didn't have that failure mode, but did have the
             * exact same "caller already got DRV_OK and never learns
             * about a later async failure" gap this now also closes).
             *
             * svc_storage_save_settings()/svc_storage_save_calibration()'s
             * synchronous return only covers whether the write could be
             * QUEUED, not whether it actually completes — callers that
             * clear settings_save_failed the moment queueing succeeds
             * (App/app_ui.c's commit_edit(), Services/svc_api.c's
             * SET_ZERO/SET_CALIBRATION/SET_SETTINGS ACKing on DRV_OK) can
             * be premature. Escalate here for BOTH kinds of save — not
             * just settings — so a calibration write that fails async
             * after its synchronous ACK is still surfaced somewhere,
             * rather than the host believing an EEPROM write succeeded
             * that never actually did. */
            g_system_state.settings_save_failed = true;
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
        /* The whole write genuinely finished — either all 5 settings
         * pages, or the single calibration page — the authoritative "did
         * it really succeed" point, as opposed to commit_edit()'s/
         * svc_api.c's optimistic synchronous clear on queueing. */
        g_system_state.settings_save_failed = false;
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
             * just scoped to this one page instead of the whole struct.
             * g_device_settings already holds the correct in-RAM default
             * either way (load_section() never touched *dest on
             * failure); reseed_blob() escalates via settings_save_failed
             * if even the write-back fails. */
            reseed_blob(sec->eeprom_addr, sec->version, dest, (uint16_t)sec->size);
        }
    }

    svc_storage_validate_settings(&g_device_settings);

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

    /* Displacement sensor S1/S2 + shared calibration (WP10) -- three
     * independent pages, see system_state.h's DisplacementSensorCal/
     * DisplacementSharedCal comment for why these aren't DeviceSettings
     * fields. Divisor-zero guard (gain/atten both appear as divisors in
     * Services/svc_displacement.c's k = atten*gain) mirrors
     * svc_storage_validate_settings()'s "belt and suspenders" reasoning
     * -- no host-writable path exists yet for these structs, so this is
     * currently only defending against a corrupt-but-CRC-valid page,
     * but a future SET-command handler for these would need to run the
     * same check on an untrusted payload before accepting it. */
    g_disp_s1_cal.gain           = DEFAULT_DISP_GAIN;
    g_disp_s1_cal.d0_mm          = DEFAULT_DISP_D0_MM;
    g_disp_s1_cal.zero_offset_mm = DEFAULT_DISP_ZERO_OFFSET_MM;
    load_or_reseed_disp_blob(EEPROM_DISP_S1_SETTINGS_ADDR, EEPROM_DISP_S1_SETTINGS_VERSION,
                             &g_disp_s1_cal, sizeof(g_disp_s1_cal));
    if (g_disp_s1_cal.gain == 0.0f) {
        g_disp_s1_cal.gain = DEFAULT_DISP_GAIN;
    }

    g_disp_s2_cal.gain           = DEFAULT_DISP_GAIN;
    g_disp_s2_cal.d0_mm          = DEFAULT_DISP_D0_MM;
    g_disp_s2_cal.zero_offset_mm = DEFAULT_DISP_ZERO_OFFSET_MM;
    load_or_reseed_disp_blob(EEPROM_DISP_S2_SETTINGS_ADDR, EEPROM_DISP_S2_SETTINGS_VERSION,
                             &g_disp_s2_cal, sizeof(g_disp_s2_cal));
    if (g_disp_s2_cal.gain == 0.0f) {
        g_disp_s2_cal.gain = DEFAULT_DISP_GAIN;
    }

    g_disp_shared_cal.atten = DEFAULT_DISP_ATTEN;
    load_or_reseed_disp_blob(EEPROM_DISP_SHARED_SETTINGS_ADDR, EEPROM_DISP_SHARED_SETTINGS_VERSION,
                             &g_disp_shared_cal, sizeof(g_disp_shared_cal));
    if (g_disp_shared_cal.atten == 0.0f) {
        g_disp_shared_cal.atten = DEFAULT_DISP_ATTEN;
    }
}

void svc_storage_validate_settings(DeviceSettings *settings)
{
    /* Belt-and-suspenders beyond load_section()'s own guarantees: a page
     * that legitimately passed CRC+version but somehow still holds a
     * zero divisor (e.g. a future bug in whatever writes this field, or
     * an untrusted SET_SETTINGS payload from svc_api.c) would otherwise
     * fault a consumer. Covers every EEPROM-backed divisor in
     * DeviceSettings, including filter_cutoff_hz_den/lm35_scale_mv_per_c
     * below, which have no consumer yet (the complementary filter and
     * LM35 driver are both future work) — guarded now anyway so whoever
     * wires either one up inherits protection instead of having to
     * remember to extend this function first. encoder_counts_per_detent,
     * vbat_scale_den, and the tmp236_seg*_den pair already have live
     * consumers: App/app_ui.c divides by the encoder one every UI tick,
     * Services/svc_battery.c and Drivers_App/drv_tmp236.c by theirs. */
    if (settings->encoder_counts_per_detent == 0U) {
        settings->encoder_counts_per_detent = DEFAULT_ENCODER_COUNTS_PER_DETENT;
    }
    if (settings->vbat_scale_den == 0U) {
        settings->vbat_scale_den = DEFAULT_VBAT_SCALE_DEN;
    }
    if (settings->tmp236_seg1_den == 0U) {
        settings->tmp236_seg1_den = DEFAULT_TMP236_SEG1_DEN;
    }
    if (settings->tmp236_seg2_den == 0U) {
        settings->tmp236_seg2_den = DEFAULT_TMP236_SEG2_DEN;
    }
    if (settings->filter_cutoff_hz_den == 0U) {
        settings->filter_cutoff_hz_den = DEFAULT_FILTER_CUTOFF_HZ_DEN;
    }
    if (settings->lm35_scale_mv_per_c == 0U) {
        settings->lm35_scale_mv_per_c = DEFAULT_LM35_SCALE_MV_PER_C;
    }
    /* Displacement sensor gain/atten divisor guards (WP10) live in
     * svc_storage_init() itself, right after each is loaded -- those
     * fields aren't DeviceSettings members (see system_state.h), so
     * they don't belong in this function's signature. */
}
