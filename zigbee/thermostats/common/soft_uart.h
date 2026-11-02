/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ZIGBEE_THERMOSTAT_SOFT_UART_H_
#define ZIGBEE_THERMOSTAT_SOFT_UART_H_

/* Initialize the board-configured, transmit-only software UART. */
int soft_uart_init(void);

/* Synchronously write a formatted log line. LF is emitted as CRLF. */
int soft_uart_log(const char *format, ...);

#endif /* ZIGBEE_THERMOSTAT_SOFT_UART_H_ */
