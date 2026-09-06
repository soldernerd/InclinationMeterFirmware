#ifndef DRV_BME280_H
#define DRV_BME280_H

#include <stdint.h>
#include "drv_common.h"

/* Bosch Sensortec BME280 combined humidity/pressure/temperature sensor
 * (datasheets/Sensor/BME280.pdf), on I2C1 -- shared with the EEPROM, see
 * pin_config.h. SDO tied to GND -> 7-bit address 0x76 (datasheet
 * "I2C Interface": "Connecting SDO to GND results in slave address
 * 1110110 (0x76)"). CSB tied to VDDIO -> I2C interface selected
 * ("CSB must be connected to VDDIO to select I2C interface"). This
 * BME280's temperature reading is entirely separate from the on-board
 * TMP236 (drv_tmp236.c) and external LM35 (TEMP_SENSE_EXT) sensors --
 * three independent temperature sources, not to be confused.
 *
 * The module sits on a header and can be unplugged or plugged in at any
 * time, not just at boot -- drv_bme280_update() below retries
 * initialisation itself whenever the module isn't currently
 * initialised (never seen at boot, or dropped out later), rather than
 * giving up permanently after the one boot-time drv_bme280_init() call.
 * Any comm failure during a normal read cycle also forces a fresh
 * re-init before the next reading is trusted, rather than assuming
 * stale calibration data and a half-written register configuration
 * are still good. */
#define BME280_I2C_ADDR   0x76U

typedef struct {
    int16_t  temp_cdeg;          /* 0.01 degC / LSB, e.g. 2345 = 23.45 degC */
    uint32_t pressure_pa;        /* Pa */
    uint16_t humidity_centipct;  /* 0.01 %RH / LSB, e.g. 4633 = 46.33 %RH */
} bme280_data_t;

/* Soft-resets the device, verifies chip_id == 0x60, and reads out the
 * factory calibration trim parameters (needed by every subsequent
 * compensation calculation) -- call once from main.c. Not the only way
 * this ever runs: drv_bme280_update() calls the same sequence again on
 * its own if the module wasn't initialised (or stopped responding), so
 * this being called just once at boot does not mean the module only
 * gets one chance. */
DrvStatus drv_bme280_init(void);

/* Triggers one forced-mode conversion (temperature+pressure+humidity,
 * oversampling and filter per config.h's BME280_*_VALUE macros), blocks
 * for the conversion to complete (bounded -- ~9.3 ms max per the
 * datasheet's own timing formula, plus hal_i2c.c's now-tightened
 * per-call I2C_TIMEOUT_MS bounding every blocking I2C transaction this
 * makes -- see config.h's comment), reads back and compensates the
 * result. If the module isn't currently initialised (never seen at
 * boot, or a previous cycle's comm failure dropped it), retries the
 * full init sequence first and, if that succeeds, falls through to
 * attempt a real reading in the same call -- the module can be plugged
 * in at any time, not just at boot, and this is what makes that work.
 * Call once per second from App/app_scheduler.c. Returns DRV_OK only if
 * THIS cycle produced a fresh reading -- callers that need to track
 * current sensor health (e.g. g_system_state.bme280_ok) must key off
 * this return value, not drv_bme280_get_result()'s DRV_OK, which only
 * means "some past cycle succeeded" and keeps returning the last good
 * reading indefinitely once the sensor stops responding. A no-op
 * (previous result kept, DRV_ERR_NOT_READY) if I2C1 is busy with an
 * EEPROM transfer this tick -- self-healing, retried next call. */
DrvStatus drv_bme280_update(void);

/* Last compensated reading -- DRV_OK here means "a reading exists," not
 * "it's current"; see drv_bme280_update()'s own return value for
 * per-cycle freshness. DRV_ERR_NOT_READY if drv_bme280_update() has
 * never completed a conversion yet. */
DrvStatus drv_bme280_get_result(bme280_data_t *out);

/* Saturating count of drv_bme280_update() cycles that failed (I2C
 * error, status-poll timeout, or skipped because the bus was busy) --
 * CLAUDE.md 7.6 escalation; no retry queue exists otherwise. */
uint16_t drv_bme280_get_error_count(void);

#endif /* DRV_BME280_H */
