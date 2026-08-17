#include "hal_uart.h"
#include "config.h"
#include "stm32g0xx_hal.h"
#include <string.h>

/* USART6 <-> RN4871. CubeMX must configure:
 *   - 115200 8N1
 *   - DMA: USART6_TX normal mode, USART6_RX circular mode (byte/byte)
 *   - NVIC: USART6 global interrupt enabled (idle line detection)
 * Generated handles live in usart.c — extern below. */
extern UART_HandleTypeDef huart6;
extern DMA_HandleTypeDef  hdma_usart6_rx;

#define RING_SIZE  UART_RX_RING_SIZE
#if (RING_SIZE & (RING_SIZE - 1)) != 0
#error "UART_RX_RING_SIZE must be a power of two"
#endif

/* DMA writes received bytes here in circular mode. The "head" is the
 * position the DMA controller will write next; we compute it from the
 * NDTR (number-of-data-to-transfer) register. The "tail" is the next
 * byte to be returned by hal_uart_read_byte — application-owned. */
static volatile uint8_t  s_rx_buf[RING_SIZE];
static volatile uint16_t s_rx_tail = 0;

static volatile bool     s_tx_busy = false;

static inline uint16_t dma_write_pos(void)
{
    /* NDTR counts down from RING_SIZE to 1, then reloads. Position is
     * therefore (RING_SIZE - NDTR) & mask. */
    uint32_t ndtr = __HAL_DMA_GET_COUNTER(&hdma_usart6_rx);
    return (uint16_t)((RING_SIZE - ndtr) & (RING_SIZE - 1U));
}

bool hal_uart_init(void)
{
    s_rx_tail = 0;
    s_tx_busy = false;

    /* Kick off circular DMA reception into the ring buffer. Reception is
     * consumed entirely via NDTR position polling (dma_write_pos()) — no
     * IDLE-line interrupt is armed. UART_IT_IDLE would need
     * HAL_UARTEx_ReceiveToIdle_DMA() (ReceptionType == HAL_UART_RECEPTION_TOIDLE)
     * to ever get cleared by the vendor HAL_UART_IRQHandler; with plain
     * HAL_UART_Receive_DMA() as used here, ReceptionType stays STANDARD and
     * the IRQ handler's IDLE-clearing branch is permanently skipped — an
     * enabled-but-never-cleared IDLE interrupt is a permanent NVIC
     * re-entry storm the moment the RN4871 UART line goes idle. */
    return HAL_UART_Receive_DMA(&huart6, (uint8_t *)s_rx_buf, RING_SIZE) == HAL_OK;
}

DrvStatus hal_uart_write(const uint8_t *data, uint16_t len)
{
    if (data == 0 || len == 0) return DRV_ERR_INVALID;
    if (s_tx_busy)             return DRV_ERR_NOT_READY;

    s_tx_busy = true;
    if (HAL_UART_Transmit_DMA(&huart6, (uint8_t *)data, len) != HAL_OK) {
        s_tx_busy = false;
        return DRV_ERR_COMM;
    }
    return DRV_OK;
}

/* KNOWN LIMITATION: no overflow detection. The classic head==tail "empty"
 * test also can't distinguish "empty" from "completely full of unread
 * bytes" — if the DMA write position ever laps all the way around to
 * s_rx_tail (RING_SIZE bytes received since the last drain), this reports
 * empty and the whole buffer's contents are silently lost, same as if the
 * DMA had overwritten unread bytes mid-buffer. At 115200 baud that's
 * ~2.2ms/byte; RING_SIZE (256, see config.h) can be exceeded within one
 * task_ble_ms scheduler tick (100ms default) under sustained transparent-
 * mode throughput. Accepted rather than fixed here: real BLE notification
 * throughput is normally well under raw UART line rate, and task_ble_ms is
 * a tunable EEPROM setting if this proves to matter on real hardware —
 * same proportionality call as the other documented WP5 gaps. */
bool hal_uart_read_byte(uint8_t *byte)
{
    if (byte == 0) return false;
    uint16_t head = dma_write_pos();
    if (head == s_rx_tail) {
        return false;
    }
    *byte = s_rx_buf[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) & (RING_SIZE - 1U));
    return true;
}

uint16_t hal_uart_bytes_available(void)
{
    uint16_t head = dma_write_pos();
    return (uint16_t)((head - s_rx_tail) & (RING_SIZE - 1U));
}

bool hal_uart_is_busy(void)
{
    return s_tx_busy;
}

/* HAL weak override — fires when DMA TX completes */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == huart6.Instance) {
        s_tx_busy = false;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == huart6.Instance) {
        s_tx_busy = false;
        /* Restart RX DMA if it stopped due to an overrun/framing error.
         * Any bytes already in the ring but not yet drained by
         * drv_rn4871_task() are lost here (s_rx_tail snaps to the fresh
         * DMA position) — accepted, undiagnosed data loss on an already-
         * rare error path; HAL_App has no system_state escalation point
         * (only App/Services write g_system_state — see CLAUDE.md 8.1),
         * and a genuine restart failure below means the RX pipe is dead
         * regardless, degrading to "no more bytes arrive" rather than a
         * crash. */
        if (HAL_UART_Receive_DMA(huart, (uint8_t *)s_rx_buf, RING_SIZE) == HAL_OK) {
            s_rx_tail = dma_write_pos();
        }
    }
}
