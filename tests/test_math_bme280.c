/* Host tests for Math/math_bme280.c: the Bosch example, the register parsing, and the integer formulas against the
 * datasheet's double-precision reference formulas (Appendix 8.1) over the whole operating range. */
#include "test.h"
#include <math.h>
#include <string.h>

#include "../Math/math_bme280.c"

/* The calibration of the datasheet's worked example (BMP280 / BME280 compensation examples). */
static MathBme280Calib example(void)
{
    MathBme280Calib c;
    memset(&c, 0, sizeof c);
    c.T1 = 27504; c.T2 = 26435; c.T3 = -1000;
    c.P1 = 36477; c.P2 = -10685; c.P3 = 3024; c.P4 = 2855; c.P5 = 140; c.P6 = -7; c.P7 = 15500; c.P8 = -14600; c.P9 = 6000;
    /* humidity trims of a typical part */
    c.H1 = 75; c.H2 = 370; c.H3 = 0; c.H4 = 331; c.H5 = 50; c.H6 = 30;
    return c;
}

/* ---- the datasheet's floating-point reference (Appendix 8.1) ---- */
static double ref_t_fine;
static double ref_temperature(const MathBme280Calib *c, double adc_T)
{
    double var1 = (adc_T / 16384.0 - c->T1 / 1024.0) * c->T2;
    double var2 = (adc_T / 131072.0 - c->T1 / 8192.0) * (adc_T / 131072.0 - c->T1 / 8192.0) * c->T3;
    ref_t_fine = var1 + var2;
    return ref_t_fine / 5120.0;
}
static double ref_pressure(const MathBme280Calib *c, double adc_P)
{
    double var1 = ref_t_fine / 2.0 - 64000.0;
    double var2 = var1 * var1 * c->P6 / 32768.0;
    var2 = var2 + var1 * c->P5 * 2.0;
    var2 = var2 / 4.0 + c->P4 * 65536.0;
    var1 = (c->P3 * var1 * var1 / 524288.0 + c->P2 * var1) / 524288.0;
    var1 = (1.0 + var1 / 32768.0) * c->P1;
    double p = 1048576.0 - adc_P;
    p = (p - var2 / 4096.0) * 6250.0 / var1;
    var1 = c->P9 * p * p / 2147483648.0;
    var2 = p * c->P8 / 32768.0;
    return p + (var1 + var2 + c->P7) / 16.0;
}
static double ref_humidity(const MathBme280Calib *c, double adc_H)
{
    double v = ref_t_fine - 76800.0;
    v = (adc_H - (c->H4 * 64.0 + c->H5 / 16384.0 * v)) *
        (c->H2 / 65536.0 * (1.0 + c->H6 / 67108864.0 * v * (1.0 + c->H3 / 67108864.0 * v)));
    v = v * (1.0 - c->H1 * v / 524288.0);
    if (v > 100.0) v = 100.0;
    if (v < 0.0) v = 0.0;
    return v;
}

TEST(the_datasheet_example_gives_25_08_degC_and_100653_pa)
{
    MathBme280Calib c = example();
    int32_t t_fine;
    int32_t t = math_bme280_temperature(&c, 519888, &t_fine);
    CHECK_EQ(t_fine, 128422);
    CHECK_EQ(t, 2508);                                        /* 25.08 degC */
    uint32_t p = math_bme280_pressure_q24_8(&c, 415148, t_fine);
    /* the datasheet quotes 100653.27 Pa; the integer variant is good to a few hundredths of a pascal */
    CHECK(fabs(p / 256.0 - 100653.27) < 0.05);
    ref_temperature(&c, 519888);
    CHECK(fabs(p / 256.0 - ref_pressure(&c, 415148)) < 0.05);       /* and agrees with the floating-point reference */
}

TEST(the_integer_formulas_match_the_double_precision_reference_over_the_operating_range)
{
    MathBme280Calib c = example();
    double worst_t = 0, worst_p = 0, worst_h = 0;
    for (int32_t adc_T = 430000; adc_T <= 600000; adc_T += 2500) {      /* about -30 .. +70 degC */
        int32_t t_fine;
        int32_t t = math_bme280_temperature(&c, adc_T, &t_fine);
        double rt = ref_temperature(&c, adc_T);
        double dt = fabs(t / 100.0 - rt);
        if (dt > worst_t) worst_t = dt;
        for (int32_t adc_P = 330000; adc_P <= 500000; adc_P += 10000) {        /* about 300 .. 1100 hPa */
            double dp = fabs(math_bme280_pressure_q24_8(&c, adc_P, t_fine) / 256.0 - ref_pressure(&c, adc_P));
            if (dp > worst_p) worst_p = dp;
        }
        for (int32_t adc_H = 20000; adc_H <= 50000; adc_H += 2000) {
            double dh = fabs(math_bme280_humidity_q22_10(&c, adc_H, t_fine) / 1024.0 - ref_humidity(&c, adc_H));
            if (dh > worst_h) worst_h = dh;
        }
    }
    CHECK(worst_t <= 0.006);              /* 0.01 degC resolution: half a step */
    CHECK(worst_p <= 1.0);                /* Pa: the datasheet's integer variant is accurate to well under a pascal */
    CHECK(worst_h <= 0.05);               /* %RH */
}

TEST(humidity_is_clamped_to_zero_and_one_hundred_percent)
{
    MathBme280Calib c = example();
    int32_t t_fine;
    math_bme280_temperature(&c, 519888, &t_fine);
    CHECK_EQ(math_bme280_humidity_q22_10(&c, 0, t_fine), 0u);
    CHECK_EQ(math_bme280_humidity_q22_10(&c, 65535, t_fine), 100u * 1024u);
}

TEST(the_pressure_guard_returns_zero_instead_of_dividing_by_zero)
{
    MathBme280Calib c = example();
    c.P1 = 0;
    CHECK_EQ(math_bme280_pressure_q24_8(&c, 415148, 128422), 0u);
}

TEST(the_calibration_registers_are_unpacked_with_the_right_signs_and_nibbles)
{
    uint8_t low[26] = {0}, high[7] = {0};
    low[0] = 0x70; low[1] = 0x6B;                             /* T1 = 27504 */
    low[2] = 0x43; low[3] = 0x67;                             /* T2 = 26435 */
    low[4] = 0x18; low[5] = 0xFC;                             /* T3 = -1000 */
    low[6] = 0x7D; low[7] = 0x8E;                             /* P1 = 36477 */
    low[8] = 0x43; low[9] = 0xD6;                             /* P2 = -10685 */
    low[22] = 0x70; low[23] = 0x17;                           /* P9 = 6000 */
    low[25] = 75;                                             /* H1 */
    high[0] = 0x72; high[1] = 0x01;                           /* H2 = 370 */
    high[2] = 0x00;                                           /* H3 */
    high[3] = 0x14;                                           /* 0xE4 */
    high[4] = 0x3B;                                           /* 0xE5: low nibble 0xB (H4 bits 3:0), high nibble 0x3 (H5 bits 3:0) */
    high[5] = 0x03;                                           /* 0xE6 */
    high[6] = 0x1E;                                           /* H6 = 30 */
    MathBme280Calib c;
    math_bme280_parse_calib(low, high, &c);
    CHECK_EQ(c.T1, 27504); CHECK_EQ(c.T2, 26435); CHECK_EQ(c.T3, -1000);
    CHECK_EQ(c.P1, 36477); CHECK_EQ(c.P2, -10685); CHECK_EQ(c.P9, 6000);
    CHECK_EQ(c.H1, 75); CHECK_EQ(c.H2, 370); CHECK_EQ(c.H6, 30);
    CHECK_EQ(c.H4, (0x14 << 4) | 0x0B);                       /* 0x14B = 331 */
    CHECK_EQ(c.H5, (0x03 << 4) | 0x03);                       /* 0x33 = 51 */
    /* negative H4 / H5: the upper register is a SIGNED byte */
    high[3] = 0xF0; high[5] = 0xE0;
    math_bme280_parse_calib(low, high, &c);
    CHECK_EQ(c.H4, (-16 * 16) | 0x0B);
    CHECK_EQ(c.H5, (-32 * 16) | 0x03);
}

int main(void)
{
    RUN(the_datasheet_example_gives_25_08_degC_and_100653_pa);
    RUN(the_integer_formulas_match_the_double_precision_reference_over_the_operating_range);
    RUN(humidity_is_clamped_to_zero_and_one_hundred_percent);
    RUN(the_pressure_guard_returns_zero_instead_of_dividing_by_zero);
    RUN(the_calibration_registers_are_unpacked_with_the_right_signs_and_nibbles);
    return test_summary();
}
