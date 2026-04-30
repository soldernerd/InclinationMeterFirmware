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
static AdcResults    s_results = {0};

void hal_adc_init(void)
{
    /* hadc1 already initialised by MX_ADC1_Init (CubeMX). Calibration is
     * required before the first conversion on STM32G0 — silent no-op if
     * calibration has already been run. */
    HAL_ADCEx_Calibration_Start(&hadc1);
    s_ready   = false;
    s_started = false;
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

const AdcResults *hal_adc_get_results(void)
{
    return &s_results;
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
    s_started = false;
    s_ready   = false;
    HAL_ADC_Stop_DMA(hadc);
}
