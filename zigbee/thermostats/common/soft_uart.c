/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>

#include "soft_uart.h"

#define SOFT_UART_NODE DT_ALIAS(softuart)
#define SOFT_UART_BAUD DT_PROP(SOFT_UART_NODE, current_speed)
#define SOFT_UART_TX_GPIO GPIO_DT_SPEC_GET(SOFT_UART_NODE, gpios)
#define SOFT_UART_BUFFER_SIZE 256U

BUILD_ASSERT(DT_NODE_HAS_STATUS(SOFT_UART_NODE, okay), "soft UART DTS node must be enabled");
BUILD_ASSERT(SOFT_UART_BAUD > 0U, "soft UART baud rate must be non-zero");
BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC >= SOFT_UART_BAUD,
	     "soft UART needs at least one system-clock cycle per bit");

static const struct gpio_dt_spec tx_gpio = SOFT_UART_TX_GPIO;
static struct k_mutex tx_mutex;
static bool initialized;

int soft_uart_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&tx_gpio)) {
		return -ENODEV;
	}

	rc = gpio_pin_configure_dt(&tx_gpio, GPIO_OUTPUT_HIGH);
	if (rc == 0) {
		k_mutex_init(&tx_mutex);
		initialized = true;
	}

	return rc;
}

static void wait_until(uint32_t deadline)
{
	while ((int32_t)(sys_clock_cycle_get_32() - deadline) < 0) {
		/* Busy wait to maintain the UART bit period. */
	}
}

static void put_byte(uint8_t byte)
{
	const uint32_t cycles_per_second = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
	const uint32_t baud = SOFT_UART_BAUD;
	uint32_t start;
	unsigned int key;

	/* Keep an interrupt from stretching a bit and corrupting the UART frame. */
	key = irq_lock();
	start = sys_clock_cycle_get_32();
	(void)gpio_pin_set_raw(tx_gpio.port, tx_gpio.pin, 0);

	for (uint32_t bit = 0U; bit < 8U; bit++) {
		uint32_t offset = ((bit + 1U) * cycles_per_second) / baud;

		wait_until(start + offset);
		(void)gpio_pin_set_raw(tx_gpio.port, tx_gpio.pin, (byte >> bit) & 1U);
	}

	wait_until(start + (9U * cycles_per_second) / baud);
	(void)gpio_pin_set_raw(tx_gpio.port, tx_gpio.pin, 1);
	wait_until(start + (10U * cycles_per_second) / baud);
	irq_unlock(key);
}

static void write_text(const char *text)
{
	bool previous_was_cr = false;

	for (; *text != '\0'; text++) {
		if (*text == '\n' && !previous_was_cr) {
			put_byte('\r');
		}
		put_byte((uint8_t)*text);
		previous_was_cr = (*text == '\r');
	}
}

int soft_uart_log(const char *format, ...)
{
	char buffer[SOFT_UART_BUFFER_SIZE];
	va_list args;
	int length;

	if (!initialized || format == NULL) {
		return -ENODEV;
	}

	va_start(args, format);
	length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (length < 0) {
		return length;
	}

	/* Logging is thread-context only and serialized to prevent interleaving. */
	k_mutex_lock(&tx_mutex, K_FOREVER);
	write_text(buffer);
	k_mutex_unlock(&tx_mutex);

	return length;
}
