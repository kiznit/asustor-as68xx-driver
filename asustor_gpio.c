// SPDX-License-Identifier: GPL-2.0
/*
 * asustor_gpio.c - GPIO-backed devices on the ASUSTOR AS68xxT mainboard.
 *
 * Owns the leds-gpio and gpio-keys-polled platform devices, plus the
 * gpiod lookup tables for disk/backup LEDs, the USB-copy button, and the
 * LCD controller enable line consumed by asustor_lcm.
 *
 * Copyright (C) 2026 Thierry Tremblay
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/gpio_keys.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/version.h>

#include "asustor_gpio.h"

#define GPIO_AMD_FCH "AMDI0030:00"

/*
 * Default trigger is the kernel's global "disk-activity": every bay LED
 * blinks on any block-device I/O. This is a "something is happening"
 * indicator, not per-bay activity. For real per-bay blinking, install
 * examples/disk-activity-leds/60-asustor-disk-leds.rules, which switches each LED to the
 * "blkdev" trigger bound to /dev/disk/by-path/...-ata-N (overriding
 * this default — writing to .../trigger atomically replaces it).
 */
#define DISK_ACT_LED(_name) {					\
	.name		 = _name ":green:disk",			\
	.default_state	 = LEDS_GPIO_DEFSTATE_ON,		\
	.default_trigger = "disk-activity",			\
}

#define DISK_ERR_LED(_name) {					\
	.name		 = _name ":red:disk",			\
	.default_state	 = LEDS_GPIO_DEFSTATE_OFF,		\
}

// clang-format off
// GPIO-controlled LEDs on AS68xx.
// Power / status / LAN LEDs are driven by the MCU (see asustor_mcu.c),
// so they do not appear here.
//
// Entries for bays 1-6 are known from the AS6806T/AS6810T Fox mainboard.
// Bays 7-10 exist on AS6810T (and AS6808T) but their GPIO pins are not
// yet identified; those LED slots are registered only up to the model's
// bay count, and only once the pins are known (see ASUSTOR_AS68XX_MAX_KNOWN_BAYS).
#define ASUSTOR_AS68XX_MAX_KNOWN_BAYS 6

static struct gpio_led asustor_as68xx_leds[] = {
	{ .name = "asustor:green:usbcopy", .default_state = LEDS_GPIO_DEFSTATE_OFF }, // 0 (backup button LED)
	{ .name = "asustor:blue:lan",      .default_state = LEDS_GPIO_DEFSTATE_ON  }, // 1
	DISK_ACT_LED("sata1"),                                                        // 2
	DISK_ERR_LED("sata1"),                                                        // 3
	DISK_ACT_LED("sata2"),                                                        // 4
	DISK_ERR_LED("sata2"),                                                        // 5
	DISK_ACT_LED("sata3"),                                                        // 6
	DISK_ERR_LED("sata3"),                                                        // 7
	DISK_ACT_LED("sata4"),                                                        // 8
	DISK_ERR_LED("sata4"),                                                        // 9
	DISK_ACT_LED("sata5"),                                                        // 10
	DISK_ERR_LED("sata5"),                                                        // 11
	DISK_ACT_LED("sata6"),                                                        // 12
	DISK_ERR_LED("sata6"),                                                        // 13
};

static struct gpio_led_platform_data asustor_as68xx_leds_pdata = {
	.leds     = asustor_as68xx_leds,
	.num_leds = 0, /* set at runtime from num_bays */
};

/*
 * Shared GPIO LED lookup table for every AS68xxT variant. All four models
 * are built on the same Rembrandt/Fox board, so bays 1-6 land on the same
 * pins. Pins for bays 7-10 are not yet known and therefore not listed.
 */
static struct gpiod_lookup_table asustor_as68xx_gpio_leds_lookup = {
	.dev_id = "leds-gpio",
	.table = {
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 147, NULL,  0, GPIO_ACTIVE_HIGH), // asustor:green:usbcopy
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 148, NULL,  1, GPIO_ACTIVE_LOW),  // asustor:blue:lan
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH,  69, NULL,  2, GPIO_ACTIVE_HIGH), // sata1:green:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH,  68, NULL,  3, GPIO_ACTIVE_LOW),  // sata1:red:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH,  75, NULL,  4, GPIO_ACTIVE_HIGH), // sata2:green:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH,  74, NULL,  5, GPIO_ACTIVE_LOW),  // sata2:red:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH,  79, NULL,  6, GPIO_ACTIVE_HIGH), // sata3:green:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH,  78, NULL,  7, GPIO_ACTIVE_LOW),  // sata3:red:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 106, NULL,  8, GPIO_ACTIVE_HIGH), // sata4:green:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 107, NULL,  9, GPIO_ACTIVE_LOW),  // sata4:red:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 146, NULL, 10, GPIO_ACTIVE_LOW),  // sata5:green:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 145, NULL, 11, GPIO_ACTIVE_HIGH), // sata5:red:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 154, NULL, 12, GPIO_ACTIVE_HIGH), // sata6:green:disk
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 156, NULL, 13, GPIO_ACTIVE_LOW),  // sata6:red:disk
		{}
	},
};

// LCD controller enable - consumed by asustor_lcm via
// gpiod_get(&pdev->dev, "enable"). Drives the LCD controller logic; the
// backlight has its own supply and is unaffected by this line.
static struct gpiod_lookup_table asustor_as68xx_gpio_lcd_lookup = {
	.dev_id = "asustor_lcm",
	.table = {
		GPIO_LOOKUP(GPIO_AMD_FCH, 4, "enable", GPIO_ACTIVE_HIGH),
		{}
	},
};
// clang-format on

// gpio-keys-polled does not consume gpio lookup tables directly; we fill in
// the absolute GPIO numbers in asustor_gpio_init() based on the lookup below.
static struct gpio_keys_button asustor_as68xx_keys_table[] = {
	{
		.desc       = "USB Copy Button",
		.code       = KEY_COPY,
		.type       = EV_KEY,
		.active_low = 1,
		.gpio       = -1, // Invalid, set in init.
	},
};

static struct gpio_keys_platform_data asustor_as68xx_keys_pdata = {
	.buttons       = asustor_as68xx_keys_table,
	.nbuttons      = ARRAY_SIZE(asustor_as68xx_keys_table),
	.poll_interval = 50,
	.name          = "asustor-keys",
};

static struct gpiod_lookup_table asustor_as68xx_gpio_keys_lookup = {
	.dev_id = "gpio-keys-polled",
	.table = {
		GPIO_LOOKUP_IDX(GPIO_AMD_FCH, 89, NULL, 0, GPIO_ACTIVE_LOW),
		// Front-panel LCD navigation buttons (1-4 + enter) are driven
		// by the MCU, see asustor_lcm.c.
		// The power button is handled via ACPI.
		{}
	},
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
static int as68xx_gpiochip_match_name(struct gpio_chip *chip, void *data)
{
	const char *name = data;

	return !strcmp(chip->label, name);
}

static int get_gpio_base_for_chipname(const char *name)
{
	struct gpio_chip *gc;

	gc = gpiochip_find((void *)name, as68xx_gpiochip_match_name);
	return gc ? gc->base : -1;
}
#else
static int get_gpio_base_for_chipname(const char *name)
{
	struct gpio_device *dev;
	int ret = -1;

	dev = gpio_device_find_by_label(name);
	if (dev) {
		ret = gpio_device_get_base(dev);
		gpio_device_put(dev);
	}
	return ret;
}
#endif

static struct platform_device *asustor_as68xx_leds_pdev;
static struct platform_device *asustor_as68xx_keys_pdev;

static struct platform_device *
asustor_gpio_create_pdev(const char *name, const void *pdata, size_t sz)
{
	struct platform_device *pdev;

	pdev = platform_device_register_data(NULL, name, PLATFORM_DEVID_NONE,
					     pdata, sz);
	if (IS_ERR(pdev))
		pr_err("failed registering %s: %ld\n", name, PTR_ERR(pdev));

	return pdev;
}

int asustor_gpio_init(unsigned int num_bays)
{
	const struct gpiod_lookup *keys_table;
	unsigned int num_disk_leds;
	int ret, i;

	/*
	 * Only register disk LEDs whose GPIO pins we actually know. Bays
	 * 7-10 on AS6808T/AS6810T are not yet mapped and are omitted rather
	 * than failing platform-device registration with -ENOENT.
	 */
	num_disk_leds = min_t(unsigned int, num_bays,
			      ASUSTOR_AS68XX_MAX_KNOWN_BAYS);
	asustor_as68xx_leds_pdata.num_leds = 2 + num_disk_leds * 2;
	if (num_bays > ASUSTOR_AS68XX_MAX_KNOWN_BAYS)
		pr_info("disk LEDs for bays %d-%u not yet supported (GPIO pins unknown)\n",
			ASUSTOR_AS68XX_MAX_KNOWN_BAYS + 1, num_bays);

	gpiod_add_lookup_table(&asustor_as68xx_gpio_leds_lookup);
	gpiod_add_lookup_table(&asustor_as68xx_gpio_keys_lookup);
	gpiod_add_lookup_table(&asustor_as68xx_gpio_lcd_lookup);

	for (i = 0; i < ARRAY_SIZE(asustor_as68xx_keys_table); i++) {
		keys_table = asustor_as68xx_gpio_keys_lookup.table;
		for (; keys_table->key; keys_table++) {
			int gpio_base;

			if (i != keys_table->idx)
				continue;

			gpio_base = get_gpio_base_for_chipname(keys_table->key);
			if (gpio_base == -1) {
				pr_warn("GPIO chip \"%s\" not found; \"%s\" disabled\n",
					keys_table->key,
					asustor_as68xx_keys_table[i].desc);
				continue;
			}

			asustor_as68xx_keys_table[i].gpio =
				gpio_base + keys_table->chip_hwnum;
		}
	}

	asustor_as68xx_leds_pdev = asustor_gpio_create_pdev(
		"leds-gpio", &asustor_as68xx_leds_pdata,
		sizeof(asustor_as68xx_leds_pdata));
	if (IS_ERR(asustor_as68xx_leds_pdev)) {
		ret = PTR_ERR(asustor_as68xx_leds_pdev);
		goto err;
	}

	asustor_as68xx_keys_pdev = asustor_gpio_create_pdev(
		"gpio-keys-polled", &asustor_as68xx_keys_pdata,
		sizeof(asustor_as68xx_keys_pdata));
	if (IS_ERR(asustor_as68xx_keys_pdev)) {
		ret = PTR_ERR(asustor_as68xx_keys_pdev);
		platform_device_unregister(asustor_as68xx_leds_pdev);
		goto err;
	}

	return 0;

err:
	gpiod_remove_lookup_table(&asustor_as68xx_gpio_leds_lookup);
	gpiod_remove_lookup_table(&asustor_as68xx_gpio_keys_lookup);
	gpiod_remove_lookup_table(&asustor_as68xx_gpio_lcd_lookup);
	return ret;
}

void asustor_gpio_cleanup(void)
{
	platform_device_unregister(asustor_as68xx_leds_pdev);
	platform_device_unregister(asustor_as68xx_keys_pdev);

	gpiod_remove_lookup_table(&asustor_as68xx_gpio_leds_lookup);
	gpiod_remove_lookup_table(&asustor_as68xx_gpio_keys_lookup);
	gpiod_remove_lookup_table(&asustor_as68xx_gpio_lcd_lookup);
}
