#ifndef SVC_UART_H
#define SVC_UART_H

/* Wired debug/VCP UART transport for svc_api (API v2) — USART3 on the
 * STDC14 / J4 debug header (PD8 TX / PD9 RX, 115200 8N1). The third API
 * transport alongside USB and BLE, and the one a host-side script can
 * always reach over the debug cable with nothing but pyserial: no VBUS
 * enumeration, no BLE central. See PythonTestCode/uart_test.py.
 *
 * A wired line has no peer-present signal, so this transport is marked
 * connected once at init and never disconnected — if nothing is listening
 * on the other end, queued frames simply DMA out into the void. */

void svc_uart_init(void);
void svc_uart_update(void);   /* pump every scheduler tick (task_uart) */

#endif /* SVC_UART_H */
