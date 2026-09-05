#include "hal_uart.h"
#include "usart.h"           /* huart6, MX_USART6_UART_Init already run in main() */
#include "stm32g0xx_hal.h"
#include <string.h>

extern DMA_HandleTypeDef hdma_usart6_rx;   /* defined in Core/Src/usart.c */

/* RX: free-running circular DMA fills s_rx_dma[]. The DMA never stops and
 * raises no interrupt we service — hal_uart_read() figures out how many
 * new bytes landed by comparing the DMA's own down-counter against where
 * we last read. As long as the ring is drained faster than USART6 fills
 * it (115200 baud = ~1150 B / 100 ms task tick; buffer is 512 B), nothing
 * is lost. Overrun (drain fell too far behind) is detected and the ring
 * is resynced to "newest 512 bytes" rather than returning corrupt data. */
#define RX_DMA_SIZE   512U   /* must be a power of two for the mask below */
#define RX_DMA_MASK   (RX_DMA_SIZE - 1U)

static uint8_t  s_rx_dma[RX_DMA_SIZE];
static uint16_t s_rx_tail;           /* next index we will read from        */
static bool     s_started;

static uint16_t dma_head(void)
{
    /* CNDTR counts DOWN from RX_DMA_SIZE to 0 as bytes arrive, wrapping. */
    uint16_t remaining = (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_usart6_rx);
    return (uint16_t)((RX_DMA_SIZE - remaining) & RX_DMA_MASK);
}

void hal_uart_init(void)
{
    /* Drive the RX DMA raw rather than via HAL_UART_Receive_DMA(): that
     * path also arms the UART parity/error interrupt, and the shared
     * USART3_4_5_6 IRQ handler would then let HAL_UART_IRQHandler tear the
     * DMA down on the first overrun/noise error (common while the RN4871
     * is still booting and not driving the line). Raw DMA + CR3.DMAR just
     * runs forever; a few bytes lost to an overrun are caught by the
     * frame CRC one layer up. No UART interrupt is enabled, so the shared
     * handler's HAL_UART_IRQHandler(&huart6) call finds nothing to do. */
    s_rx_tail = 0;
    s_started = false;

    __HAL_UART_DISABLE_IT(&huart6, UART_IT_PE | UART_IT_ERR | UART_IT_RXNE);

    if (HAL_DMA_Start(&hdma_usart6_rx,
                      (uint32_t)&huart6.Instance->RDR,
                      (uint32_t)s_rx_dma,
                      RX_DMA_SIZE) != HAL_OK) {
        return;
    }
    __HAL_DMA_DISABLE_IT(&hdma_usart6_rx, DMA_IT_HT | DMA_IT_TC | DMA_IT_TE);
    SET_BIT(huart6.Instance->CR3, USART_CR3_DMAR);
    s_started = true;
}

DrvStatus hal_uart_write(const uint8_t *data, uint16_t len)
{
    if (!s_started || data == 0 || len == 0) {
        return DRV_ERR_INVALID;
    }
    /* 64 bytes at 115200 8N1 is ~5.5 ms; 50 ms is generous headroom and
     * still bounded (never HAL_MAX_DELAY, which could wedge the scheduler
     * if the peripheral faults). */
    HAL_StatusTypeDef rc = HAL_UART_Transmit(&huart6, (uint8_t *)data, len, 50U);
    if (rc == HAL_TIMEOUT) {
        return DRV_ERR_TIMEOUT;
    }
    return (rc == HAL_OK) ? DRV_OK : DRV_ERR_COMM;
}

uint16_t hal_uart_rx_available(void)
{
    if (!s_started) {
        return 0;
    }
    return (uint16_t)((dma_head() - s_rx_tail) & RX_DMA_MASK);
}

uint16_t hal_uart_read(uint8_t *dst, uint16_t maxlen)
{
    if (!s_started || dst == 0 || maxlen == 0) {
        return 0;
    }
    /* Error IT is disabled, so clear a stuck overrun here instead. In DMA
     * receive mode the flag is informational — the DMA keeps reading RDR
     * regardless — but clearing it keeps the status register tidy. */
    if (__HAL_UART_GET_FLAG(&huart6, UART_FLAG_ORE)) {
        __HAL_UART_CLEAR_OREFLAG(&huart6);
    }
    uint16_t avail = hal_uart_rx_available();

    /* Overrun guard: if the DMA lapped us (drain fell more than a buffer
     * behind), the "available" count is meaningless. Resync to the most
     * recent full buffer minus a small margin and carry on — a few
     * dropped bytes beats returning a scrambled ring. */
    if (avail > (RX_DMA_SIZE - 16U)) {
        s_rx_tail = (uint16_t)((dma_head() - (RX_DMA_SIZE - 16U)) & RX_DMA_MASK);
        avail     = hal_uart_rx_available();
    }

    uint16_t n = (avail < maxlen) ? avail : maxlen;
    for (uint16_t i = 0; i < n; i++) {
        dst[i]    = s_rx_dma[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1U) & RX_DMA_MASK);
    }
    return n;
}

void hal_uart_rx_flush(void)
{
    if (s_started) {
        s_rx_tail = dma_head();
    }
}
