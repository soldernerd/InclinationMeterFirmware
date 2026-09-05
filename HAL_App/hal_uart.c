#include "hal_uart.h"
#include "usart.h"           /* huart3/huart6 + their DMA handles, MX_*_Init already run */
#include "stm32g0xx_hal.h"
#include <string.h>

/* Two instances, same code path, different peripheral + DMA channels.
 * Handles are defined in Core/Src/usart.c. */
extern UART_HandleTypeDef huart6;
extern UART_HandleTypeDef huart3;
extern DMA_HandleTypeDef  hdma_usart6_rx;
extern DMA_HandleTypeDef  hdma_usart6_tx;
extern DMA_HandleTypeDef  hdma_usart3_rx;
extern DMA_HandleTypeDef  hdma_usart3_tx;

static UART_HandleTypeDef *const s_huart[HAL_UART_COUNT] = {
    [HAL_UART_BLE]   = &huart6,
    [HAL_UART_DEBUG] = &huart3,
};
static DMA_HandleTypeDef *const s_hdma_rx[HAL_UART_COUNT] = {
    [HAL_UART_BLE]   = &hdma_usart6_rx,
    [HAL_UART_DEBUG] = &hdma_usart3_rx,
};
static DMA_HandleTypeDef *const s_hdma_tx[HAL_UART_COUNT] = {
    [HAL_UART_BLE]   = &hdma_usart6_tx,
    [HAL_UART_DEBUG] = &hdma_usart3_tx,
};

/* RX: free-running circular DMA fills s_rx_dma[inst]. The DMA never stops
 * and raises no interrupt we service — hal_uart_read() works out how many
 * new bytes landed by comparing the DMA down-counter against where we
 * last read. As long as the ring is drained faster than the line fills it
 * (115200 baud = ~1150 B / 100 ms; ring is 512 B) nothing is lost; a
 * drain that falls a whole buffer behind is detected and resynced to the
 * newest bytes rather than returning a scrambled ring. */
#define RX_DMA_SIZE   512U           /* power of two for the mask */
#define RX_DMA_MASK   (RX_DMA_SIZE - 1U)
#define TX_DMA_MAX    255U           /* CNDTR is 16-bit but our frames are <=128 */

static uint8_t  s_rx_dma[HAL_UART_COUNT][RX_DMA_SIZE];
static uint16_t s_rx_tail[HAL_UART_COUNT];
static bool     s_started[HAL_UART_COUNT];

static uint16_t dma_head(HalUartInstance inst)
{
    /* CNDTR counts DOWN from RX_DMA_SIZE to 0 as bytes arrive, wrapping. */
    uint16_t remaining = (uint16_t)__HAL_DMA_GET_COUNTER(s_hdma_rx[inst]);
    return (uint16_t)((RX_DMA_SIZE - remaining) & RX_DMA_MASK);
}

void hal_uart_init(HalUartInstance inst)
{
    if (inst >= HAL_UART_COUNT) {
        return;
    }

    s_rx_tail[inst] = 0;
    s_started[inst] = false;

    /* Drive both DMA directions raw rather than via HAL_UART_*_DMA():
     * that path arms the UART parity/error interrupt, and the shared
     * USART3_4_5_6 IRQ handler would then let HAL_UART_IRQHandler tear the
     * RX DMA down on the first overrun/noise error (common while the
     * RN4871 is still booting and not driving the line). Raw DMA + the
     * CR3 DMAR/DMAT enable bits just run; a few bytes lost to an overrun
     * are caught by the frame CRC one layer up. No UART or DMA interrupt
     * is enabled, so the shared handler's HAL_UART_IRQHandler() calls
     * find nothing to do. */
    __HAL_UART_DISABLE_IT(s_huart[inst], UART_IT_PE | UART_IT_ERR | UART_IT_RXNE |
                                         UART_IT_TXE | UART_IT_TC);

    /* The RX ring is free-running, so the channel must be circular. Only
     * re-Init when the .ioc left it otherwise (USART3_RX is Normal from
     * CubeMX; USART6_RX is already Circular — left untouched so the
     * hardware-validated BLE path is not perturbed). The channel is
     * disabled and the handle READY here (MspInit just ran HAL_DMA_Init). */
    if (s_hdma_rx[inst]->Init.Mode != DMA_CIRCULAR) {
        s_hdma_rx[inst]->Init.Mode = DMA_CIRCULAR;
        if (HAL_DMA_Init(s_hdma_rx[inst]) != HAL_OK) {
            return;
        }
    }

    if (HAL_DMA_Start(s_hdma_rx[inst],
                      (uint32_t)&s_huart[inst]->Instance->RDR,
                      (uint32_t)s_rx_dma[inst],
                      RX_DMA_SIZE) != HAL_OK) {
        return;
    }
    __HAL_DMA_DISABLE_IT(s_hdma_rx[inst], DMA_IT_HT | DMA_IT_TC | DMA_IT_TE);
    __HAL_DMA_DISABLE_IT(s_hdma_tx[inst], DMA_IT_HT | DMA_IT_TC | DMA_IT_TE);
    SET_BIT(s_huart[inst]->Instance->CR3, USART_CR3_DMAR);

    s_started[inst] = true;
}

bool hal_uart_tx_idle(HalUartInstance inst)
{
    if (inst >= HAL_UART_COUNT || !s_started[inst]) {
        return false;
    }
    /* CNDTR hits 0 once every byte has been handed to the UART; safe to
     * load the next transfer then (a still-shifting last byte does not
     * block a fresh DMA — it paces on TXE). */
    return __HAL_DMA_GET_COUNTER(s_hdma_tx[inst]) == 0U;
}

DrvStatus hal_uart_write(HalUartInstance inst, const uint8_t *data, uint16_t len)
{
    if (inst >= HAL_UART_COUNT || !s_started[inst] || data == 0 || len == 0) {
        return DRV_ERR_INVALID;
    }
    if (len > TX_DMA_MAX) {
        return DRV_ERR_INVALID;
    }
    if (!hal_uart_tx_idle(inst)) {
        return DRV_ERR_NOT_READY;
    }

    DMA_Channel_TypeDef *ch = s_hdma_tx[inst]->Instance;
    __HAL_DMA_DISABLE(s_hdma_tx[inst]);
    __HAL_DMA_CLEAR_FLAG(s_hdma_tx[inst],
        (__HAL_DMA_GET_TC_FLAG_INDEX(s_hdma_tx[inst]) |
         __HAL_DMA_GET_HT_FLAG_INDEX(s_hdma_tx[inst]) |
         __HAL_DMA_GET_TE_FLAG_INDEX(s_hdma_tx[inst])));
    ch->CPAR  = (uint32_t)&s_huart[inst]->Instance->TDR;
    ch->CMAR  = (uint32_t)data;
    ch->CNDTR = len;
    __HAL_DMA_ENABLE(s_hdma_tx[inst]);
    SET_BIT(s_huart[inst]->Instance->CR3, USART_CR3_DMAT);
    return DRV_OK;
}

uint16_t hal_uart_rx_available(HalUartInstance inst)
{
    if (inst >= HAL_UART_COUNT || !s_started[inst]) {
        return 0;
    }
    return (uint16_t)((dma_head(inst) - s_rx_tail[inst]) & RX_DMA_MASK);
}

uint16_t hal_uart_read(HalUartInstance inst, uint8_t *dst, uint16_t maxlen)
{
    if (inst >= HAL_UART_COUNT || !s_started[inst] || dst == 0 || maxlen == 0) {
        return 0;
    }
    /* Error IT is disabled, so clear a stuck overrun here instead — in DMA
     * receive mode it is informational (the DMA keeps reading RDR), but
     * clearing it keeps the status register tidy. */
    if (__HAL_UART_GET_FLAG(s_huart[inst], UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_OREFLAG(s_huart[inst]);
    }
    uint16_t avail = hal_uart_rx_available(inst);

    /* Overrun guard: if the DMA lapped us, "available" is meaningless.
     * Resync to the most recent full buffer minus a small margin — a few
     * dropped bytes beats a scrambled ring. */
    if (avail > (RX_DMA_SIZE - 16U)) {
        s_rx_tail[inst] = (uint16_t)((dma_head(inst) - (RX_DMA_SIZE - 16U)) & RX_DMA_MASK);
        avail           = hal_uart_rx_available(inst);
    }

    uint16_t n = (avail < maxlen) ? avail : maxlen;
    for (uint16_t i = 0; i < n; i++) {
        dst[i]          = s_rx_dma[inst][s_rx_tail[inst]];
        s_rx_tail[inst] = (uint16_t)((s_rx_tail[inst] + 1U) & RX_DMA_MASK);
    }
    return n;
}

void hal_uart_rx_flush(HalUartInstance inst)
{
    if (inst < HAL_UART_COUNT && s_started[inst]) {
        s_rx_tail[inst] = dma_head(inst);
    }
}
