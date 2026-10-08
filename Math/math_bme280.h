#ifndef MATH_BME280_H
#define MATH_BME280_H

/* Bosch BME280 trimming-parameter parsing and the integer compensation formulas (datasheet 4.2.2 / 4.2.3, the 32-bit and
 * 64-bit integer variants), moved out of Drivers_App/drv_bme280.c so they run on the host. Host-tested against the
 * datasheet's own example and against its double-precision reference formulas (tests/test_math_bme280.c). */

#include <stdint.h>

typedef struct {
    uint16_t T1;  int16_t T2, T3;
    uint16_t P1;  int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1;  int16_t H2;  uint8_t H3;  int16_t H4, H5;  int8_t H6;
} MathBme280Calib;

/* low = registers 0x88..0xA1 (26 bytes), high = registers 0xE1..0xE7 (7 bytes). */
void math_bme280_parse_calib(const uint8_t low[26], const uint8_t high[7], MathBme280Calib *c);

/* Temperature in 0.01 degC; also returns t_fine, which the pressure and humidity formulas need. */
int32_t  math_bme280_temperature(const MathBme280Calib *c, int32_t adc_T, int32_t *t_fine);

/* Pressure in Pa as unsigned Q24.8 (divide by 256); 0 on the datasheet's division-by-zero guard. */
uint32_t math_bme280_pressure_q24_8(const MathBme280Calib *c, int32_t adc_P, int32_t t_fine);

/* Relative humidity in %RH as unsigned Q22.10 (divide by 1024), clamped to 0 .. 100 %. */
uint32_t math_bme280_humidity_q22_10(const MathBme280Calib *c, int32_t adc_H, int32_t t_fine);

#endif /* MATH_BME280_H */
