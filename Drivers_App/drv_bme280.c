#include "drv_bme280.h"
#include "hal_i2c.h"
#include "hal_systick.h"
#include "math_bme280.h"
#include "config.h"
#include <string.h>
#include <stdbool.h>

/* Register addresses (datasheet Table 18 "Memory map"). */
#define REG_CALIB00     0x88U   /* 26 bytes: dig_T1..dig_T3, dig_P1..dig_P9, dig_H1 */
#define REG_ID          0xD0U
#define REG_RESET       0xE0U
#define REG_CALIB26     0xE1U   /* 7 bytes: dig_H2..dig_H6 (odd nibble packing, see below) */
#define REG_CTRL_HUM    0xF2U
#define REG_STATUS      0xF3U
#define REG_CTRL_MEAS   0xF4U
#define REG_CONFIG      0xF5U
#define REG_DATA_START  0xF7U   /* 8 bytes: press_msb..hum_lsb, burst read */

#define CHIP_ID_VALUE   0x60U
#define RESET_CMD       0xB6U
#define STATUS_MEASURING_BIT  0x08U
#define STATUS_IM_UPDATE_BIT  0x01U

/* Bounded blocking wait for a forced-mode conversion -- datasheet
 * Appendix B 9.1 gives t_measure_max ~= 9.3 ms at this driver's
 * oversampling settings (config.h). Generous margin, not a tight budget
 * worth hand-timing (same reasoning as WP7/WP8's boot-only delays). */
#define STATUS_POLL_TIMEOUT_MS  20U

/* Factory calibration trim parameters (datasheet 4.2.2 "Trimming
 * parameter readout", Table 16) -- read once at init, used by every
 * subsequent compensation calculation. Types/names match the datasheet's
 * own compensation formulas (4.2.3) exactly, to keep that code
 * transcribable without translation. */
static MathBme280Calib s_cal;

static bme280_data_t s_last_result;
static bool          s_have_result = false;
static bool          s_initialized = false;
static uint16_t      s_error_count = 0;

static void note_error(void)
{
    if (s_error_count < UINT16_MAX) {
        s_error_count++;
    }
}

/* An actual BME280 comm failure (write NAK'd, read failed, status never
 * cleared) -- as opposed to note_error()'s other callers, which just
 * mean "try again next cycle" (shared-bus contention, not-yet-connected).
 * Forces a full try_init() before the next reading is trusted again,
 * rather than assuming stale calibration data and a half-written
 * register configuration are still good -- covers both an outright
 * unplug and a bus glitch severe enough that resuming without a clean
 * reset would be a gamble. */
static void note_comm_failure(void)
{
    note_error();
    s_initialized = false;
}

static DrvStatus write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return hal_i2c_write(HAL_I2C_MAIN, BME280_I2C_ADDR, buf, 2U);
}

static DrvStatus read_regs(uint8_t reg, uint8_t *buf, uint16_t len)
{
    return hal_i2c_write_read(HAL_I2C_MAIN, BME280_I2C_ADDR, &reg, 1U, buf, len);
}

/* Bosch's official fixed-point compensation formulas (datasheet 4.2.3),
 * transcribed directly rather than re-derived -- the datasheet itself
 * strongly advises against hand-rewriting these. t_fine carries the
 * fine-resolution temperature value over into the pressure/humidity
 * formulas, exactly as documented. */
static int32_t t_fine;

/* Full reset+chip-ID+calibration-readout sequence -- the actual body of
 * drv_bme280_init() below, factored out so drv_bme280_update() can also
 * call it to retry a first-time or lapsed initialisation. The module is
 * on a header and can be unplugged/plugged back at any time; this must
 * be safely re-runnable, not just a one-shot boot step. */
static DrvStatus try_init(void)
{
    if (write_reg(REG_RESET, RESET_CMD) != DRV_OK) {
        return DRV_ERR_COMM;
    }
    /* t_startup max 2 ms (datasheet Table 1) before first communication;
     * generous margin, one-time boot cost. */
    hal_systick_delay_ms(3);

    /* im_update clears once the post-reset NVM->image-register copy is
     * done -- wait for it before trusting the calibration read below
     * (datasheet 5.4.4: "Automatically set to 1 when the NVM data are
     * being copied... 0 when the copying is done. The data are copied
     * at power-on-reset and before every conversion."). */
    uint32_t start = hal_systick_get_ms();
    uint8_t  status;
    for (;;) {
        if (read_regs(REG_STATUS, &status, 1U) != DRV_OK) {
            return DRV_ERR_COMM;
        }
        if ((status & STATUS_IM_UPDATE_BIT) == 0U) {
            break;
        }
        if (hal_systick_elapsed_ms(start) >= STATUS_POLL_TIMEOUT_MS) {
            /* NVM->register copy never finished -- do not proceed to
             * read (possibly still-copying, garbage) calibration data;
             * a real device should never hit this given the datasheet's
             * documented copy time is far under this timeout. */
            return DRV_ERR_TIMEOUT;
        }
    }

    uint8_t chip_id = 0;
    if (read_regs(REG_ID, &chip_id, 1U) != DRV_OK) {
        return DRV_ERR_COMM;
    }
    if (chip_id != CHIP_ID_VALUE) {
        return DRV_ERR_COMM;
    }

    uint8_t calib_low[26];
    if (read_regs(REG_CALIB00, calib_low, sizeof(calib_low)) != DRV_OK) {
        return DRV_ERR_COMM;
    }
    uint8_t calib_high[7];
    if (read_regs(REG_CALIB26, calib_high, sizeof(calib_high)) != DRV_OK) {
        return DRV_ERR_COMM;
    }
    math_bme280_parse_calib(calib_low, calib_high, &s_cal);

    s_initialized = true;
    return DRV_OK;
}

DrvStatus drv_bme280_init(void)
{
    s_have_result = false;
    s_error_count = 0;
    return try_init();
}

/* Hard wall-time ceiling for one drv_bme280_update() call (config.h).
 * Checked at every phase boundary. A single in-flight blocking I2C op
 * can't be interrupted, so the true worst case is this budget plus one
 * hal_i2c I2C_TIMEOUT_MS — still bounded, unlike the old unbounded stack
 * of per-op timeouts on a clock-stretching sensor. */
static bool over_budget(uint32_t t0)
{
    return hal_systick_elapsed_ms(t0) >= BME280_UPDATE_BUDGET_MS;
}

DrvStatus drv_bme280_update(void)
{
    const uint32_t t0 = hal_systick_get_ms();

    if (!s_initialized) {
        /* Never initialised (module absent/unresponsive at boot -- see
         * main.c, which calls drv_bme280_init() but doesn't halt if it
         * fails), or a previous cycle's comm failure below forced a
         * fresh re-init. Either way, retry here rather than giving up
         * permanently -- the module sits on a header and can be
         * plugged in (or back in) at any point during runtime, not
         * just at boot. hal_i2c_is_busy() is checked first since a
         * retry attempt is exactly as bus-sensitive as a normal read
         * cycle. */
        if (hal_i2c_is_busy(HAL_I2C_MAIN)) {
            note_error();
            return DRV_ERR_NOT_READY;
        }
        if (try_init() != DRV_OK) {
            note_error();
            return DRV_ERR_NOT_READY;
        }
        if (over_budget(t0)) {
            /* try_init() succeeded but ate the budget — take the reading
             * next tick rather than pushing this call past the ceiling. */
            return DRV_ERR_NOT_READY;
        }
        /* Fall through to attempt a real reading in this same cycle --
         * minimizes the delay between plugging the module in and the
         * first reported reading, rather than waiting a further second
         * for the next scheduler tick. */
    }
    if (hal_i2c_is_busy(HAL_I2C_MAIN)) {
        /* EEPROM DMA transfer in flight on the shared bus -- skip this
         * cycle rather than blocking-write on top of it; self-healing,
         * retried on the next scheduler tick. */
        note_error();
        return DRV_ERR_NOT_READY;
    }

    /* ctrl_hum must be written before ctrl_meas for the oversampling
     * change to take effect (datasheet 5.4.3: "Changes to this register
     * only become effective after a write operation to ctrl_meas"). The
     * ctrl_meas write below also triggers the forced-mode conversion. */
    if (write_reg(REG_CTRL_HUM, BME280_CTRL_HUM_VALUE) != DRV_OK) {
        note_comm_failure();
        return DRV_ERR_COMM;
    }
    if (write_reg(REG_CONFIG, BME280_CONFIG_VALUE) != DRV_OK) {
        note_comm_failure();
        return DRV_ERR_COMM;
    }
    if (write_reg(REG_CTRL_MEAS, BME280_CTRL_MEAS_VALUE) != DRV_OK) {
        note_comm_failure();
        return DRV_ERR_COMM;
    }
    if (over_budget(t0)) {
        note_comm_failure();
        return DRV_ERR_TIMEOUT;
    }

    /* Small fixed delay before the first status check: the "measuring"
     * bit isn't guaranteed set the instant the ctrl_meas write above
     * completes (there's a real, if brief, window before the device's
     * internal state machine catches up), and the conversion itself
     * takes ~9.3 ms regardless (datasheet Appendix B 9.1) -- so this
     * costs nothing in the common case while closing that window,
     * rather than risking the poll below seeing stale idle status and
     * reading back the *previous* conversion's data as if it were
     * fresh. */
    hal_systick_delay_ms(2);

    uint8_t status;
    for (;;) {
        if (read_regs(REG_STATUS, &status, 1U) != DRV_OK) {
            note_comm_failure();
            return DRV_ERR_COMM;
        }
        if ((status & STATUS_MEASURING_BIT) == 0U) {
            break;
        }
        if (over_budget(t0)) {
            /* Conversion not done within the per-call budget (should be
             * ~9.3 ms on a healthy part) -- give up and force a fresh
             * re-init before trusting this module again, same reasoning
             * as note_comm_failure()'s other callers. Deliberately a
             * blocking poll rather than a multi-tick state machine
             * (unlike drv_24lc256.c's EEPROM write-cycle poll) --
             * justified by the hard BME280_UPDATE_BUDGET_MS ceiling. */
            note_comm_failure();
            return DRV_ERR_TIMEOUT;
        }
    }

    uint8_t data[8];
    if (read_regs(REG_DATA_START, data, sizeof(data)) != DRV_OK) {
        note_comm_failure();
        return DRV_ERR_COMM;
    }

    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    int32_t adc_H = ((int32_t)data[6] << 8) | data[7];

    int32_t  temp_c01   = math_bme280_temperature(&s_cal, adc_T, &t_fine);          /* 0.01 degC */
    uint32_t pressure_q24_8 = math_bme280_pressure_q24_8(&s_cal, adc_P, t_fine);      /* Q24.8 Pa */
    uint32_t humidity_q22_10 = math_bme280_humidity_q22_10(&s_cal, adc_H, t_fine);     /* Q22.10 %RH */

    s_last_result.temp_cdeg          = (int16_t)temp_c01;
    s_last_result.pressure_pa        = pressure_q24_8 >> 8;
    s_last_result.humidity_centipct  = (uint16_t)((humidity_q22_10 * 100U) >> 10);
    s_have_result = true;
    return DRV_OK;
}

DrvStatus drv_bme280_get_result(bme280_data_t *out)
{
    if (out == 0) {
        return DRV_ERR_INVALID;
    }
    if (!s_have_result) {
        return DRV_ERR_NOT_READY;
    }
    memcpy(out, &s_last_result, sizeof(*out));
    return DRV_OK;
}

