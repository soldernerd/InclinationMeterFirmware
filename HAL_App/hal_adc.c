#include "hal_adc.h"
#include "stm32g0xx_hal.h"

extern ADC_HandleTypeDef hadc1;

#define SCAN_LEN  4U

/* DMA target. ADC1 uses the NOT_FULLY_CONFIGURABLE (fixed) sequencer on this
 * part — every enabled channel gets ADC_RANK_CHANNEL_NUMBER, so hardware
 * always converts in ASCENDING CHANNEL NUMBER order regardless of the order
 * channels were configured in CubeMX. The real, fixed scan order is:
 *   [0] CH3  (PA3 / BATTERY_SENSE)
 *   [1] CH13 (VREFINT)
 *   [2] CH15 (PB11 / TEMP_SENSE_EXT)
 *   [3] CH16 (PB12 / TEMP_SENSE)
 * NOT the old order (VREFINT, VBAT, TEMP) from the 3-channel REV A scan —
 * see docs/cubemx_configuration_checklist.md §8.
 * 32-bit aligned to satisfy DMA half-word transfers. */
static volatile uint16_t s_dma_buf[SCAN_LEN];

static volatile bool s_ready = false;
static volatile bool s_started = false;
static adc_results_t s_results = {0};

bool hal_adc_init(void)
{
    /* hadc1 already initialised by MX_ADC1_Init (CubeMX). Calibration is
     * required before the first conversion on STM32G0 — a failure here
     * means every subsequent reading (battery voltage, both temperature
     * channels) would be silently wrong, so the caller must check this.
     *
     * Retry a few times: on a fast wake from Standby the 3V3 rail (= VREF+
     * on REV B) may still be settling when we first get here, and
     * calibration on a low VREF fails. A short spaced retry rides that
     * out; a hard, persistent failure still returns false. */
    bool ok = false;
    for (uint8_t attempt = 0; attempt < 5U && !ok; ++attempt) {
        if (attempt != 0U) {
            HAL_Delay(10);
        }
        ok = (HAL_ADCEx_Calibration_Start(&hadc1) == HAL_OK);
    }
    s_ready   = false;
    s_started = false;
    return ok;
}

void hal_adc_start(void)
{
    if (s_started) {
        /* Previous scan still running — let it finish; caller polls is_ready */
        return;
    }
    s_ready = false;
    s_started = true;
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)s_dma_buf, SCAN_LEN) != HAL_OK) {
        s_started = false;
    }
}

bool hal_adc_is_ready(void)
{
    return s_ready;
}

adc_results_t hal_adc_get_results(void)
{
    /* Snapshot atomically w.r.t. HAL_ADC_ConvCpltCallback — s_results is
     * four fields updated together by the ISR; without this, a caller
     * could read one field, get preempted by a new scan completing, then
     * read another field from the new scan, silently mixing two different
     * scans' data. */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    adc_results_t snapshot = s_results;
    __set_PRIMASK(primask);
    return snapshot;
}

uint32_t hal_adc_raw_to_mv(uint16_t channel_raw, uint16_t vrefint_raw)
{
    if (vrefint_raw == 0U) {
        return 0U;   /* no valid scan yet — avoid divide-by-zero */
    }
    /* Standard STM32 VREFINT-ratiometric conversion: the factory-calibrated
     * VREFINT_CAL value tells us what a 3.000 V reference reads as on this
     * exact chip; comparing that to what VREFINT actually reads *this scan*
     * gives the true current VDDA, which every other channel in the same
     * scan is relative to. Oversampling (16x, right-shift 4, see ADC1 init)
     * keeps every channel on the same 12-bit numeric scale as VREFINT_CAL
     * itself, so no rescaling is needed beyond this. */
    uint32_t vdda_mv = ((uint32_t)VREFINT_CAL_VREF * (*VREFINT_CAL_ADDR)) / vrefint_raw;
    return ((uint32_t)channel_raw * vdda_mv) / 4095U;
}

uint16_t hal_adc_read_vrefint_raw(void)  { return s_results.vrefint_raw; }
uint16_t hal_adc_read_vbat_raw(void)     { return s_results.vbat_raw; }
uint16_t hal_adc_read_temp_raw(void)     { return s_results.temp_raw; }
uint16_t hal_adc_read_temp_ext_raw(void) { return s_results.temp_ext_raw; }

/* HAL weak override — fires on DMA full-buffer complete. With circular
 * mode disabled (Normal mode), this fires once per scan triggered by
 * hal_adc_start(). */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != hadc1.Instance) {
        return;
    }
    s_results.vbat_raw     = s_dma_buf[0];   /* CH3  / PA3  / BATTERY_SENSE */
    s_results.vrefint_raw  = s_dma_buf[1];   /* CH13 / VREFINT */
    s_results.temp_ext_raw = s_dma_buf[2];   /* CH15 / PB11 / TEMP_SENSE_EXT */
    s_results.temp_raw     = s_dma_buf[3];   /* CH16 / PB12 / TEMP_SENSE */
    s_results.valid        = true;
    s_started              = false;
    s_ready                = true;
    HAL_ADC_Stop_DMA(hadc);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != hadc1.Instance) {
        return;
    }
    s_started        = false;
    s_ready          = false;
    /* Without this, consumers gating on .valid (svc_battery.c,
     * drv_tmp236.c) would keep trusting the last-good reading forever —
     * .valid is only ever set true in HAL_ADC_ConvCpltCallback and had
     * nothing clearing it back to false on a persistent fault. */
    s_results.valid  = false;
    HAL_ADC_Stop_DMA(hadc);
}
