#ifndef SVC_BLE_H
#define SVC_BLE_H

#include <stdbool.h>

void svc_ble_init(void);
void svc_ble_task(void);        /* pump every task_ble tick */
bool svc_ble_is_connected(void);

#endif /* SVC_BLE_H */
