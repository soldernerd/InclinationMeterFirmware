/* Host tests for Services/svc_storage.c -- the REAL source against a fake 24LC256: first boot on a blank chip, load of saved
 * settings, per-page version and CRC protection (a bad page is reseeded, the others are left alone), the asynchronous save
 * with retries, a write that never completes, restore-defaults and the divisor guards. The calibrations live here, so this
 * is the code whose silent failure costs the most. */
#include "test.h"
#include <string.h>

#include "../Math/math_crc.c"
#include "../Config/config.h"
#include "../system_state.h"
#include "../Drivers_App/drv_24lc256.h"

DeviceSettings g_device_settings;
SystemState    g_system_state;

/* ---------------- doubles ---------------- */
static uint32_t g_ms;
uint32_t hal_systick_get_ms(void) { return g_ms; }
uint32_t hal_systick_elapsed_ms(uint32_t s) { return g_ms - s; }

#include "../Services/svc_log.h"
static int g_warns;
void svc_log(Api2LogSeverity s, const char *m) { (void)m; if (s == API2_LOG_WARN) g_warns++; }
void svc_logf(Api2LogSeverity s, const char *f, ...) { (void)f; if (s == API2_LOG_WARN) g_warns++; }

/* the fake chip */
static uint8_t g_mem[EEPROM_TOTAL_BYTES];
static bool    g_busy, g_last_ok;
static int     g_delay_updates = 0, g_left;
static int     g_fail_writes;               /* the next N writes fail (NAK) */
static int     g_stuck_ops;                 /* the next N operations never finish by themselves */
static bool    g_stuck_now;
static int     g_writes, g_aborts;
static Lc256WriteFail g_fail_kind;

void drv_24lc256_init(void) { g_busy = false; }
bool drv_24lc256_is_busy(void) { return g_busy; }
bool drv_24lc256_read_complete(void) { return g_last_ok; }
bool drv_24lc256_write_complete(void) { return g_last_ok; }
Lc256WriteFail drv_24lc256_last_write_fail(void) { return g_fail_kind; }
void drv_24lc256_abort(void) { g_busy = false; g_stuck_now = false; g_last_ok = false; g_aborts++; }   /* like the real driver: an aborted op did not succeed */
void drv_24lc256_update(void)
{
    g_ms++;
    if (g_busy && !g_stuck_now && --g_left <= 0) g_busy = false;
}
static void begin_op(void)
{
    g_busy = true;
    g_left = g_delay_updates + 1;
    g_stuck_now = false;
    if (g_stuck_ops > 0) { g_stuck_ops--; g_stuck_now = true; }
}
DrvStatus drv_24lc256_start_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (g_busy) return DRV_ERR_NOT_READY;
    if ((uint32_t)addr + len > EEPROM_TOTAL_BYTES) return DRV_ERR_INVALID;
    memcpy(buf, &g_mem[addr], len);
    g_last_ok = true;
    begin_op();
    return DRV_OK;
}
DrvStatus drv_24lc256_start_write_page(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    if (g_busy) return DRV_ERR_NOT_READY;
    if (((addr & (EEPROM_PAGE_SIZE - 1U)) + len) > EEPROM_PAGE_SIZE) return DRV_ERR_INVALID;   /* never cross a page */
    g_writes++;
    g_fail_kind = LC256_WRITE_OK;
    if (g_fail_writes > 0) {
        g_fail_writes--;
        g_last_ok = false;
        g_fail_kind = LC256_WRITE_DMA_NAK;
    } else {
        memcpy(&g_mem[addr], buf, len);
        g_last_ok = true;
    }
    begin_op();
    return DRV_OK;
}

#include "../Services/svc_storage.c"

/* ---------------- helpers ---------------- */
static void chip_blank(void)
{
    memset(g_mem, 0xFF, sizeof g_mem);
    g_busy = false; g_delay_updates = 0; g_fail_writes = 0; g_stuck_ops = 0; g_stuck_now = false;
    g_writes = g_aborts = 0; g_warns = 0; g_ms = 5000;
    memset(&g_device_settings, 0x5A, sizeof g_device_settings);   /* garbage: init must overwrite it */
    memset(&g_system_state, 0, sizeof g_system_state);
    memset(&s_pending, 0, sizeof s_pending);
    s_busy_seen = false;
}

static void pump_until_idle(int max_ms)
{
    for (int i = 0; i < max_ms && svc_storage_is_busy(); ++i) {
        g_ms++;
        svc_storage_update();
    }
}

static const SettingsSection *section_of(const void *field)
{
    size_t off = (size_t)((const uint8_t *)field - (const uint8_t *)&g_device_settings);
    for (uint8_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
        if (off >= s_sections[i].offset && off < s_sections[i].offset + s_sections[i].size) return &s_sections[i];
    }
    return 0;
}

static uint16_t page_crc_in_mem(const SettingsSection *sec)
{
    return (uint16_t)(g_mem[sec->eeprom_addr + 4] | (g_mem[sec->eeprom_addr + 5] << 8));
}

static DeviceSettings defaults(void)
{
    DeviceSettings d;
    fill_default_settings(&d);
    return d;
}

/* ---------------- tests ---------------- */

TEST(a_blank_chip_is_seeded_with_defaults_and_every_page_gets_a_valid_header)
{
    chip_blank();
    svc_storage_init();
    DeviceSettings d = defaults();
    CHECK(memcmp(&g_device_settings, &d, sizeof d) == 0);
    CHECK_EQ(g_system_state.eeprom_selftest, 0);
    CHECK_EQ(g_system_state.settings_save_failed, 0);
    for (uint8_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
        const SettingsSection *s = &s_sections[i];
        CHECK_EQ(g_mem[s->eeprom_addr], 0xA5);
        CHECK_EQ(g_mem[s->eeprom_addr + 1], 0x5A);
        CHECK_EQ(g_mem[s->eeprom_addr + 2] | (g_mem[s->eeprom_addr + 3] << 8), s->version);
        CHECK_EQ(page_crc_in_mem(s), math_crc16((const uint8_t *)&g_device_settings + s->offset, (uint16_t)s->size));
    }
    CHECK(g_warns >= SETTINGS_SECTION_COUNT);                /* each reseed was logged */
}

/* A reboot: the RAM is lost, the chip keeps its contents. */
static void ram_reboot(void)
{
    memset(&g_device_settings, 0x5A, sizeof g_device_settings);
    memset(&g_system_state, 0, sizeof g_system_state);
    memset(&s_pending, 0, sizeof s_pending);
    s_busy_seen = false;
    g_busy = false;
    g_warns = 0;
}

TEST(saved_settings_survive_a_reboot_exactly)
{
    chip_blank();
    svc_storage_init();
    g_device_settings.disp_s1_k_micro = 23456;
    g_device_settings.disp_s2_zero_ppm = -9876;
    g_device_settings.disp_s1_phase_cdeg = -612;
    g_device_settings.auto_poweroff_s = 777;
    g_device_settings.vbat_offset_mv = -42;
    g_device_settings.rtc_trim_ppm_x10 = 321;
    CHECK_EQ(svc_storage_save_settings(&g_device_settings), DRV_OK);
    CHECK(svc_storage_is_busy());
    pump_until_idle(2000);
    CHECK(!svc_storage_is_busy());
    CHECK(!g_system_state.settings_save_failed);
    DeviceSettings saved = g_device_settings;
    static uint8_t image[EEPROM_TOTAL_BYTES];
    memcpy(image, g_mem, sizeof image);

    ram_reboot();
    svc_storage_init();
    CHECK_EQ(g_warns, 0);                                    /* nothing needed reseeding */
    CHECK(memcmp(&g_device_settings, &saved, sizeof saved) == 0);
    CHECK(memcmp(g_mem, image, 0x700) == 0);                 /* and nothing was rewritten (below the self-test area) */
}

TEST(a_wrong_version_page_is_reseeded_and_the_other_pages_are_untouched)
{
    chip_blank();
    svc_storage_init();
    g_device_settings.disp_s1_k_micro = 31111;               /* displacement page */
    g_device_settings.tmp236_seg1_num = 4321;                /* tmp236 page */
    g_device_settings.battery_low_mv  = 3650;                /* battery page */
    svc_storage_save_settings(&g_device_settings);
    pump_until_idle(2000);

    const SettingsSection *bat = section_of(&g_device_settings.battery_low_mv);
    g_mem[bat->eeprom_addr + 2] ^= 0x01;                     /* an older / newer layout version */
    memset(&g_device_settings, 0x5A, sizeof g_device_settings);
    g_warns = 0;
    svc_storage_init();
    DeviceSettings d = defaults();
    CHECK_EQ(g_device_settings.battery_low_mv, d.battery_low_mv);       /* reseeded */
    CHECK_EQ(g_device_settings.disp_s1_k_micro, 31111);                 /* untouched */
    CHECK_EQ(g_device_settings.tmp236_seg1_num, 4321);
    CHECK_EQ(g_warns, 1);
    CHECK_EQ(g_mem[bat->eeprom_addr + 2] | (g_mem[bat->eeprom_addr + 3] << 8), bat->version);   /* repaired on the chip */
}

TEST(a_flipped_bit_in_one_page_reseeds_only_that_page)
{
    chip_blank();
    svc_storage_init();
    g_device_settings.disp_s2_k_micro = 27777;
    g_device_settings.lm35_scale_mv_per_c = 11;
    svc_storage_save_settings(&g_device_settings);
    pump_until_idle(2000);

    const SettingsSection *lm = section_of(&g_device_settings.lm35_scale_mv_per_c);
    g_mem[lm->eeprom_addr + 6] ^= 0x04;                      /* a bit rots in the data */
    memset(&g_device_settings, 0x5A, sizeof g_device_settings);
    g_warns = 0;
    svc_storage_init();
    CHECK_EQ(g_device_settings.lm35_scale_mv_per_c, DEFAULT_LM35_SCALE_MV_PER_C);
    CHECK_EQ(g_device_settings.disp_s2_k_micro, 27777);
    CHECK_EQ(g_warns, 1);

    /* a flipped bit in the CRC bytes themselves, and in the magic, are caught the same way */
    const SettingsSection *dp = section_of(&g_device_settings.disp_s2_k_micro);
    g_mem[dp->eeprom_addr + 4] ^= 0x80;
    memset(&g_device_settings, 0x5A, sizeof g_device_settings);
    svc_storage_init();
    CHECK_EQ(g_device_settings.disp_s2_k_micro, DEFAULT_DISP_S2_K_MICRO);
    g_mem[dp->eeprom_addr] = 0x00;
    memset(&g_device_settings, 0x5A, sizeof g_device_settings);
    svc_storage_init();
    CHECK_EQ(g_device_settings.disp_s2_k_micro, DEFAULT_DISP_S2_K_MICRO);
}

TEST(a_save_while_one_is_running_is_refused_not_mixed)
{
    chip_blank();
    svc_storage_init();
    g_delay_updates = 3;
    CHECK_EQ(svc_storage_save_settings(&g_device_settings), DRV_OK);
    CHECK(svc_storage_is_busy());
    CHECK(svc_storage_save_settings(&g_device_settings) != DRV_OK);
    pump_until_idle(5000);
    CHECK(!svc_storage_is_busy());
}

TEST(a_failed_page_write_is_retried_and_succeeds)
{
    chip_blank();
    svc_storage_init();
    g_writes = 0;
    g_fail_writes = 2;                                       /* two NAKs, below the retry limit of 3 */
    g_device_settings.disp_s1_zero_ppm = 4242;
    svc_storage_save_settings(&g_device_settings);
    pump_until_idle(5000);
    CHECK(!g_system_state.settings_save_failed);
    const SettingsSection *dp = section_of(&g_device_settings.disp_s1_zero_ppm);
    CHECK_EQ(page_crc_in_mem(dp), math_crc16((const uint8_t *)&g_device_settings + dp->offset, (uint16_t)dp->size));
    CHECK(g_writes > SETTINGS_SECTION_COUNT);                /* the retries happened */
}

TEST(a_write_that_keeps_failing_gives_up_and_latches_the_failure)
{
    chip_blank();
    svc_storage_init();
    g_fail_writes = 100;
    svc_storage_save_settings(&g_device_settings);
    pump_until_idle(5000);
    CHECK(!svc_storage_is_busy());                           /* it did not wedge */
    CHECK(g_system_state.settings_save_failed);
    g_fail_writes = 0;
    CHECK_EQ(svc_storage_save_settings(&g_device_settings), DRV_OK);   /* and a later save works again */
    pump_until_idle(5000);
    CHECK(!g_system_state.settings_save_failed);             /* success clears the latch */
}

TEST(an_operation_that_never_finishes_is_aborted_after_the_timeout)
{
    chip_blank();
    svc_storage_init();
    g_aborts = 0;
    g_stuck_ops = 1;                                         /* the first chunk hangs on the bus */
    svc_storage_save_settings(&g_device_settings);
    pump_until_idle(5000);
    CHECK(!svc_storage_is_busy());
    CHECK(g_aborts >= 1);                                    /* the stuck transfer was cancelled ... */
    CHECK(!g_system_state.settings_save_failed);             /* ... and the retry completed the save */

    g_stuck_ops = 100;                                       /* a bus that stays dead */
    svc_storage_save_settings(&g_device_settings);
    pump_until_idle(20000);
    CHECK(!svc_storage_is_busy());
    CHECK(g_system_state.settings_save_failed);
}

TEST(restore_defaults_overwrites_ram_and_chip)
{
    chip_blank();
    svc_storage_init();
    g_device_settings.disp_s1_k_micro = 99999;
    g_device_settings.disp_s1_phase_cdeg = 1234;
    g_device_settings.battery_critical_mv = 3000;
    svc_storage_save_settings(&g_device_settings);
    pump_until_idle(2000);
    CHECK_EQ(svc_storage_restore_defaults(), DRV_OK);
    DeviceSettings d = defaults();
    CHECK(memcmp(&g_device_settings, &d, sizeof d) == 0);    /* effective at once */
    pump_until_idle(2000);
    memset(&g_device_settings, 0x5A, sizeof g_device_settings);
    svc_storage_init();
    CHECK(memcmp(&g_device_settings, &d, sizeof d) == 0);    /* and persisted */
    g_delay_updates = 5;
    svc_storage_save_settings(&g_device_settings);
    CHECK(svc_storage_restore_defaults() != DRV_OK);         /* refused while a save runs */
    pump_until_idle(5000);
}

TEST(divisors_and_ranges_are_guarded)
{
    chip_blank();
    DeviceSettings s = defaults();
    s.encoder_counts_per_detent = 0;  s.vbat_scale_den = 0;
    s.tmp236_seg1_den = 0;  s.tmp236_seg2_den = 0;  s.lm35_scale_mv_per_c = 0;
    s.disp_s1_k_micro = 0;  s.disp_s2_k_micro = -5;
    s.rtc_trim_ppm_x10 = 6000;
    svc_storage_validate_settings(&s);
    CHECK_EQ(s.encoder_counts_per_detent, DEFAULT_ENCODER_COUNTS_PER_DETENT);
    CHECK_EQ(s.vbat_scale_den, DEFAULT_VBAT_SCALE_DEN);
    CHECK_EQ(s.tmp236_seg1_den, DEFAULT_TMP236_SEG1_DEN);
    CHECK_EQ(s.tmp236_seg2_den, DEFAULT_TMP236_SEG2_DEN);
    CHECK_EQ(s.lm35_scale_mv_per_c, DEFAULT_LM35_SCALE_MV_PER_C);
    CHECK_EQ(s.disp_s1_k_micro, DEFAULT_DISP_S1_K_MICRO);
    CHECK_EQ(s.disp_s2_k_micro, DEFAULT_DISP_S2_K_MICRO);
    CHECK_EQ(s.rtc_trim_ppm_x10, DEFAULT_RTC_TRIM_PPM_X10);
    s.rtc_trim_ppm_x10 = -4800;                              /* in range: kept */
    svc_storage_validate_settings(&s);
    CHECK_EQ(s.rtc_trim_ppm_x10, -4800);
}

TEST(a_corrupt_page_with_a_zero_divisor_cannot_reach_a_consumer)
{
    chip_blank();
    svc_storage_init();
    /* a page that passes magic, version and CRC but holds a zero divisor (a bug elsewhere wrote it) */
    const SettingsSection *enc = section_of(&g_device_settings.encoder_counts_per_detent);
    g_device_settings.encoder_counts_per_detent = 0;
    uint8_t page[HDR_SIZE + 64];
    uint16_t crc = math_crc16((const uint8_t *)&g_device_settings + enc->offset, (uint16_t)enc->size);
    build_header(page, enc->version, crc);
    memcpy(&page[HDR_SIZE], (const uint8_t *)&g_device_settings + enc->offset, enc->size);
    memcpy(&g_mem[enc->eeprom_addr], page, HDR_SIZE + enc->size);
    memset(&g_device_settings, 0x5A, sizeof g_device_settings);
    svc_storage_init();
    CHECK(g_device_settings.encoder_counts_per_detent != 0);
}

TEST(every_page_has_its_own_address_and_fits_its_slot)
{
    for (uint8_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
        CHECK(s_sections[i].size > 0);
        CHECK(HDR_SIZE + s_sections[i].size <= 0x100u);
        CHECK(s_sections[i].eeprom_addr + 0x100u <= EEPROM_SELFTEST_ADDR);          /* below the self-test scratch area */
        for (uint8_t j = (uint8_t)(i + 1); j < SETTINGS_SECTION_COUNT; ++j) {
            CHECK(s_sections[i].eeprom_addr != s_sections[j].eeprom_addr);
        }
    }
}

int main(void)
{
    RUN(a_blank_chip_is_seeded_with_defaults_and_every_page_gets_a_valid_header);
    RUN(saved_settings_survive_a_reboot_exactly);
    RUN(a_wrong_version_page_is_reseeded_and_the_other_pages_are_untouched);
    RUN(a_flipped_bit_in_one_page_reseeds_only_that_page);
    RUN(a_save_while_one_is_running_is_refused_not_mixed);
    RUN(a_failed_page_write_is_retried_and_succeeds);
    RUN(a_write_that_keeps_failing_gives_up_and_latches_the_failure);
    RUN(an_operation_that_never_finishes_is_aborted_after_the_timeout);
    RUN(restore_defaults_overwrites_ram_and_chip);
    RUN(divisors_and_ranges_are_guarded);
    RUN(a_corrupt_page_with_a_zero_divisor_cannot_reach_a_consumer);
    RUN(every_page_has_its_own_address_and_fits_its_slot);
    return test_summary();
}
