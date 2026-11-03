/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/printk-hooks.h>

#define SOFT_UART_NODE DT_ALIAS(softuart)
#define SOFT_UART_BAUD DT_PROP(SOFT_UART_NODE, current_speed)
#define SOFT_UART_TX_GPIO GPIO_DT_SPEC_GET(SOFT_UART_NODE, gpios)

BUILD_ASSERT(DT_NODE_HAS_STATUS(SOFT_UART_NODE, okay), "soft UART DTS node must be enabled");
BUILD_ASSERT(SOFT_UART_BAUD > 0U, "soft UART baud rate must be non-zero");
BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC >= SOFT_UART_BAUD,
	     "soft UART needs at least one system-clock cycle per bit");

static const struct gpio_dt_spec tx_gpio = SOFT_UART_TX_GPIO;
static bool initialized;
static printk_hook_fn_t downstream_printk_hook;

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

static int soft_uart_printk_char_out(int character)
{
	static bool previous_was_cr;
	uint8_t byte = (uint8_t)character;

	if (!initialized) {
		return character;
	}

	if (byte == '\n' && !previous_was_cr) {
		put_byte('\r');
	}
	put_byte(byte);
	previous_was_cr = (byte == '\r');
	if (downstream_printk_hook != NULL) {
		(void)downstream_printk_hook(character);
	}

	return character;
}

static int soft_uart_printk_init(void)
{
	int rc;

	if (!gpio_is_ready_dt(&tx_gpio)) {
		return -ENODEV;
	}

	rc = gpio_pin_configure_dt(&tx_gpio, GPIO_OUTPUT_HIGH);
	if (rc == 0) {
		initialized = true;
		/* Keep an existing console (for example RTT) as a mirror. */
		downstream_printk_hook = __printk_get_hook();
		__printk_hook_install(soft_uart_printk_char_out);
		__stdout_hook_install(soft_uart_printk_char_out);
	}

	return rc;
}

/* GPIO is ready after PRE_KERNEL_1; install before APPLICATION boot banner. */
SYS_INIT(soft_uart_printk_init, PRE_KERNEL_2, 1);
