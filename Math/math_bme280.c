#include "math_bme280.h"

void math_bme280_parse_calib(const uint8_t low[26], const uint8_t high[7], MathBme280Calib *c)
{
    c->T1 = (uint16_t)(low[0]  | (low[1]  << 8));
    c->T2 = (int16_t)(low[2]  | (low[3]  << 8));
    c->T3 = (int16_t)(low[4]  | (low[5]  << 8));
    c->P1 = (uint16_t)(low[6]  | (low[7]  << 8));
    c->P2 = (int16_t)(low[8]  | (low[9]  << 8));
    c->P3 = (int16_t)(low[10] | (low[11] << 8));
    c->P4 = (int16_t)(low[12] | (low[13] << 8));
    c->P5 = (int16_t)(low[14] | (low[15] << 8));
    c->P6 = (int16_t)(low[16] | (low[17] << 8));
    c->P7 = (int16_t)(low[18] | (low[19] << 8));
    c->P8 = (int16_t)(low[20] | (low[21] << 8));
    c->P9 = (int16_t)(low[22] | (low[23] << 8));
    /* low[24] = register 0xA0, reserved (the gap between the pressure trims and dig_H1) */
    c->H1 = low[25];   /* register 0xA1 */

    c->H2 = (int16_t)(high[0] | (high[1] << 8));
    c->H3 = high[2];
    /* dig_H4 / dig_H5 are two 12-bit values that share register 0xE5's nibbles (datasheet Table 16); transcribed from
     * Bosch's own reference driver, since this layout is a well-known source of errors. */
    c->H4 = (int16_t)(((int8_t)high[3] * 16) | (high[4] & 0x0FU));
    c->H5 = (int16_t)(((int8_t)high[5] * 16) | (high[4] >> 4));
    c->H6 = (int8_t)high[6];
}

int32_t math_bme280_temperature(const MathBme280Calib *c, int32_t adc_T, int32_t *t_fine)
{
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)c->T1 << 1))) * ((int32_t)c->T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)c->T1)) * ((adc_T >> 4) - ((int32_t)c->T1))) >> 12) *
            ((int32_t)c->T3)) >> 14;
    *t_fine = var1 + var2;
    return (*t_fine * 5 + 128) >> 8;
}

uint32_t math_bme280_pressure_q24_8(const MathBme280Calib *c, int32_t adc_P, int32_t t_fine)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)c->P6;
    var2 = var2 + ((var1 * (int64_t)c->P5) << 17);
    var2 = var2 + (((int64_t)c->P4) << 35);
    var1 = ((var1 * var1 * (int64_t)c->P3) >> 8) + ((var1 * (int64_t)c->P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)c->P1) >> 33;
    if (var1 == 0) {
        return 0;   /* avoid the division by zero */
    }
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)c->P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)c->P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)c->P7) << 4);
    return (uint32_t)p;
}

uint32_t math_bme280_humidity_q22_10(const MathBme280Calib *c, int32_t adc_H, int32_t t_fine)
{
    int32_t v;
    v = (t_fine - ((int32_t)76800));
    v = (((((adc_H << 14) - (((int32_t)c->H4) << 20) - (((int32_t)c->H5) * v)) + ((int32_t)16384)) >> 15) *
         (((((((v * ((int32_t)c->H6)) >> 10) * (((v * ((int32_t)c->H3)) >> 11) + ((int32_t)32768))) >> 10) +
            ((int32_t)2097152)) * ((int32_t)c->H2) + 8192) >> 14));
    v = (v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)c->H1)) >> 4));
    v = (v < 0 ? 0 : v);
    v = (v > 419430400 ? 419430400 : v);
    return (uint32_t)(v >> 12);
}
