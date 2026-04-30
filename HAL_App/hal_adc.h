#ifndef HAL_ADC_H
#define HAL_ADC_H

#include <stdint.h>
#include <stdbool.h>

/* ADC1's fixed sequencer always converts CH3, CH13(VREFINT), CH15, CH16 in
 * that ascending-channel-number order — see hal_adc.c. */
typedef struct {
    uint16_t vbat_raw;      /* CH3  / PA3  / BATTERY_SENSE */
    uint16_t vrefint_raw;   /* CH13 / internal reference */
    uint16_t temp_ext_raw;  /* CH15 / PB11 / TEMP_SENSE_EXT */
    uint16_t temp_raw;      /* CH16 / PB12 / TEMP_SENSE */
    bool     valid;         /* true after first DMA scan completes */
} AdcResults;

void              hal_adc_init(void);
void              hal_adc_start(void);          /* trigger one DMA scan */
bool              hal_adc_is_ready(void);       /* true when DMA scan complete */
const AdcResults *hal_adc_get_results(void);

uint16_t          hal_adc_read_vbat_raw(void);
uint16_t          hal_adc_read_temp_raw(void);
uint16_t          hal_adc_read_temp_ext_raw(void);
uint16_t          hal_adc_read_vrefint_raw(void);

#endif /* HAL_ADC_H */
